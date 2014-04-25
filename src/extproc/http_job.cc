// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "extproc/http_job.hpp"

#include <stdint.h>
#include <cmath>
#include <limits>
#include "debug.hpp"

#include "containers/archive/boost_types.hpp"
#include "containers/archive/stl_types.hpp"
#include "extproc/extproc_job.hpp"
#include "rdb_protocol/rdb_protocol_json.hpp"
#include "rdb_protocol/pseudo_time.hpp"

// Returns an empty counted_t on error.
counted_t<const ql::datum_t> http_to_datum(const std::string &json);
http_result_t perform_http(const http_opts_t &opts);

// The job_t runs in the context of the main rethinkdb process
http_job_t::http_job_t(extproc_pool_t *pool, signal_t *interruptor) :
    extproc_job(pool, &worker_fn, interruptor) { }

http_result_t http_job_t::http(const http_opts_t *opts) {
    write_message_t msg;
    msg << *opts;
    {
        int res = send_write_message(extproc_job.write_stream(), &msg);
        if (res != 0) { throw http_worker_exc_t("failed to send data to the worker"); }
    }

    http_result_t result;
    archive_result_t res = deserialize(extproc_job.read_stream(), &result);
    if (bad(res)) {
        throw http_worker_exc_t(strprintf("failed to deserialize result from worker (%s)",
                                          archive_result_as_str(res)));
    }
    return result;
}

void http_job_t::worker_error() {
    extproc_job.worker_error();
}

bool http_job_t::worker_fn(read_stream_t *stream_in, write_stream_t *stream_out) {
    http_opts_t opts;
    {
        archive_result_t res = deserialize(stream_in, &opts);
        if (bad(res)) { return false; }
    }

    http_result_t result;

    try {
        result = perform_http(opts);
    } catch (const std::exception &ex) {
        result = std::string(ex.what());
    } catch (...) {
        result = std::string("Unknown error when performing http");
    }

    write_message_t msg;
    msg << result;
    int res = send_write_message(stream_out, &msg);
    if (res != 0) { return false; }

    return true;
}

std::string exec_curl(const std::string &url,
                      fd_t res_pipe, fd_t err_pipe) {
    int res;
    do {
        res = ::dup2(res_pipe, STDOUT_FILENO);
    } while (res == -1 && get_errno() == EINTR);
    
    if (res == -1) {
        return "failed to redirect stdout: " + errno_string(get_errno());
    }
    
    do {
        res = ::dup2(err_pipe, STDERR_FILENO);
    } while (res == -1 && get_errno() == EINTR);
    
    if (res == -1) {
        return "failed to redirect stderr: " + errno_string(get_errno());
    }

    std::vector<std::string> args;
    args.push_back(""); // zero arg isn't parsed, it seems
    args.push_back(std::string("--location"));
    args.push_back(std::string("--silent"));
    args.push_back(std::string("--show-error"));
    args.push_back(url);

    scoped_array_t<scoped_array_t<char> > scoped_args(args.size());
    scoped_array_t<char *> cstr_args(args.size() + 1);

    for (size_t i = 0; i < args.size(); ++i) {
        scoped_args[i].init(args[i].length() + 1);
        strncpy(scoped_args[i].data(), args[i].c_str(), args[i].length() + 1);
        cstr_args[i] = scoped_args[i].data();
    }

    cstr_args[args.size()] = NULL;
    
    do {
        res = ::execvp("curl", cstr_args.data());
    } while (res == -1 && get_errno() == EINTR);

    return "failed to exec curl: " + errno_string(get_errno());
}

void read_pipes(fd_t res_pipe, std::string *res_str,
                fd_t err_pipe, std::string *err_str) {
    int res = 0;
    char buffer[256];
    // TODO: do these in parallel so neither pipe blocks

    while (true) {
        res = ::read(res_pipe, &buffer[0], sizeof(buffer));
        if (res > 0) {
            res_str->append(buffer, res);
        } else if (res == 0) {
            break;
        } else if (get_errno() != EINTR) {
            err_str->assign(strprintf("Error when reading result from http: %s",
                                      errno_string(get_errno()).c_str()));
            return;
        }
    }

    while (true) {
        res = ::read(err_pipe, &buffer[0], sizeof(buffer));
        if (res > 0) {
            err_str->append(buffer, res);
        } else if (res == 0) {
            break;
        } else if (get_errno() != EINTR) {
            err_str->assign(strprintf("Error when reading errors from http: %s",
                                          errno_string(get_errno()).c_str()));
            return;
        }
    }

}

http_result_t perform_http(const http_opts_t &opts) {
    http_result_t result;

    fd_t res_pipe[2];
    if (::pipe(&res_pipe[0]) != 0) {
        return std::string("failed to create pipe for http stdout redirect");
    }

    fd_t err_pipe[2];
    if (::pipe(&err_pipe[0]) != 0) {
        return std::string("failed to create pipe for http stderr redirect");
    }

    int res;
    do {
        res = ::fork();
    } while (res == -1 && get_errno() == EINTR);

    if (res > 0) {
        ::close(res_pipe[0]);
        ::close(err_pipe[0]);

        // If we return from this call, something terrible has happened
        std::string info = exec_curl(opts.url, res_pipe[1], err_pipe[1]);

        do {
            res = ::write(err_pipe[1], info.c_str(), info.length());
        } while (res == -1 && get_errno() == EINTR);

        do {
            res = ::fsync(err_pipe[1]);
        } while (res == -1 && get_errno() == EINTR);

        ::exit(0);
    }
    ::close(res_pipe[1]);
    ::close(err_pipe[1]);

    scoped_fd_t res_in(res_pipe[0]);
    scoped_fd_t err_in(err_pipe[0]);
    std::string res_str;
    std::string err_str;

    read_pipes(res_in.get(), &res_str, err_in.get(), &err_str);

    if (res_str.length() == 0) {
        result = err_str;
    } else {
        counted_t<const ql::datum_t> datum = http_to_datum(res_str);
        if (datum.has()) {
            result = datum;
        } else {
            result = "invalid json: " + res_str;
        }
    }

    return result;
}

counted_t<const ql::datum_t> http_to_datum(const std::string &json) {
    scoped_cJSON_t cjson(cJSON_Parse(json.c_str()));
    if (cjson.get() == NULL) {
        return counted_t<const ql::datum_t>();
    }

    return make_counted<const ql::datum_t>(cjson);
}
