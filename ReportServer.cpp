#ifdef WITH_POSTGRESQL

#include "ReportServer/ReportServer.hpp"

#include "apostol/application.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol
{

// --- on_start ----------------------------------------------------------------

void ReportServer::on_start(EventLoop& /*loop*/, Application& app)
{
    pool_   = &app.db_pool();
    logger_ = &app.logger();

    // Create BotSession for apibot authentication
    bot_ = std::make_unique<BotSession>(*pool_, "ReportServer/2.0", "127.0.0.1");

    // Read OAuth2 credentials from conf/oauth2/default.json -> "service" app
    auto [client_id, client_secret] = app.providers().credentials("service");
    if (!client_id.empty())
        bot_->set_credentials(std::move(client_id), std::move(client_secret));

    // Subscribe to LISTEN "report" for immediate dispatch
    pool_->listen("report", [this](std::string_view /*channel*/, std::string_view payload) {
        on_notify(payload);
    });

    // Read config
    if (auto* cfg = app.module_config("ReportServer")) {
        if (cfg->contains("heartbeat") && (*cfg)["heartbeat"].is_number())
            check_interval_ = milliseconds((*cfg)["heartbeat"].get<int>());
        if (cfg->contains("report_timeout") && (*cfg)["report_timeout"].is_number())
            report_timeout_ = milliseconds((*cfg)["report_timeout"].get<int>());
        if (cfg->contains("max_in_flight") && (*cfg)["max_in_flight"].is_number())
            max_in_flight_ = (*cfg)["max_in_flight"].get<std::size_t>();
        if (cfg->contains("max_pending") && (*cfg)["max_pending"].is_number())
            max_pending_ = (*cfg)["max_pending"].get<std::size_t>();
    }

    logger_->notice("ReportServer started (check_interval={}ms, max_in_flight={}, max_pending={})",
                    check_interval_.count(), max_in_flight_, max_pending_);
}

// --- heartbeat ---------------------------------------------------------------

void ReportServer::heartbeat(std::chrono::system_clock::time_point now)
{
    if (!bot_ || !pool_)
        return;

    bot_->refresh_if_needed();

    if (status_ == Status::stopped) {
        if (bot_->valid())
            status_ = Status::running;
        return;
    }

    // Status::running
    process_notify_queue();

    if (now >= next_check_) {
        check_reports();
        next_check_ = now + check_interval_;
    }

    // Sweep stale reports (stuck in-flight longer than report_timeout_)
    for (auto it = reports_.begin(); it != reports_.end(); ) {
        auto age = std::chrono::duration_cast<milliseconds>(now - it->second.started_at);
        if (age > report_timeout_) {
            logger_->warn("ReportServer: report {} timed out after {}ms, removing",
                         it->first, age.count());
            it = reports_.erase(it);
        } else {
            ++it;
        }
    }
}

// --- on_stop -----------------------------------------------------------------

void ReportServer::on_stop()
{
    if (pool_)
        pool_->unlisten("report");
    if (bot_)
        bot_->sign_out();
    bot_.reset();
}

// --- on_notify ---------------------------------------------------------------
//
// Called from PgPool listener when a NOTIFY arrives on the "report" channel.
// Payload format: {"session": "...", "id": "<report-uuid>"}
//

void ReportServer::on_notify(std::string_view payload)
{
    try {
        auto j = nlohmann::json::parse(payload);
        if (j.contains("id") && j["id"].is_string()) {
            auto id = j["id"].get<std::string>();
            if (!id.empty() && !in_progress(id) && pending_reports_.size() < max_pending_)
                pending_reports_.push_back(std::move(id));
        }
    } catch (const nlohmann::json::exception& e) {
        if (logger_)
            logger_->error("ReportServer: bad NOTIFY payload: {}", e.what());
    }
}

// --- process_notify_queue ----------------------------------------------------

void ReportServer::process_notify_queue()
{
    if (pending_reports_.empty())
        return;

    // Move out to avoid re-entrancy issues
    auto pending = std::move(pending_reports_);
    pending_reports_.clear();

    for (auto& id : pending) {
        if (reports_.size() >= max_in_flight_)
            break;
        if (!in_progress(id))
            do_check(id);
    }
}

// --- check_reports -----------------------------------------------------------
//
// Polling fallback (every check_interval_):
//   1. api.authorize(session)
//   2. api.report_ready('enabled') ORDER BY created
//

void ReportServer::check_reports()
{
    if (!bot_->valid())
        return;

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.report_ready('enabled') ORDER BY created",
        pq_quote_literal(bot_->session()));

    pool_->execute(sql,
        [this](std::vector<PgResult> results) {
            enum_reports(std::move(results));
        },
        [this](std::string_view error) {
            on_fatal(std::string(error));
        },
        /*quiet=*/true);
}

// --- enum_reports ------------------------------------------------------------
//
// Mirrors v1 CReportServer::EnumReportReady():
//   For each report in results:
//     - state == "progress" && !in_progress -> do_start
//     - state == "canceled" && in_progress  -> do_abort
//

void ReportServer::enum_reports(std::vector<PgResult> results)
{
    // results[0] = authorize, results[1] = report_ready list
    if (results.size() < 2 || !results[1].ok())
        return;

    auto& res = results[1];
    int rows = res.rows();

    int col_id        = res.column_index("id");
    int col_statecode = res.column_index("statecode");

    if (col_id < 0 || col_statecode < 0)
        return;

    for (int r = 0; r < rows; ++r) {
        std::string id    = res.value(r, col_id)        ? res.value(r, col_id)        : "";
        std::string state = res.value(r, col_statecode) ? res.value(r, col_statecode) : "";

        if (id.empty())
            continue;

        if (in_progress(id)) {
            if (state == "canceled")
                do_abort(id);
        } else {
            if (state == "progress" && reports_.size() < max_in_flight_)
                do_start(id);
        }
    }
}

// --- do_check ----------------------------------------------------------------
//
// Verify a single report (from NOTIFY) before starting:
//   api.authorize(session) + api.get_report_ready(id)
//   If statecode == "progress" -> do_start
//

void ReportServer::do_check(const std::string& id)
{
    if (!bot_->valid())
        return;

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT id, statecode FROM api.get_report_ready({}::uuid)",
        pq_quote_literal(bot_->session()),
        pq_quote_literal(id));

    pool_->execute(sql,
        [this, id](std::vector<PgResult> results) {
            if (results.size() < 2 || !results[1].ok())
                return;

            auto& res = results[1];
            if (res.rows() == 0)
                return;

            int col_statecode = res.column_index("statecode");
            if (col_statecode < 0)
                return;

            std::string state = res.value(0, col_statecode)
                                    ? res.value(0, col_statecode) : "";
            if (state == "progress" && !in_progress(id))
                do_start(id);
        },
        [this](std::string_view error) {
            on_fatal(std::string(error));
        });
}

// --- do_start ----------------------------------------------------------------
//
// Execute the report:
//   api.authorize(session) + api.execute_report_ready(id)
//

void ReportServer::do_start(const std::string& id)
{
    reports_[id] = Report{id, std::chrono::system_clock::now()};

    logger_->debug("ReportServer: starting report {}", id);

    if (!bot_->valid()) {
        delete_report(id);
        return;
    }

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_report_ready({}::uuid)",
        pq_quote_literal(bot_->session()),
        pq_quote_literal(id));

    auto qid = pool_->execute(sql,
        [this, id](std::vector<PgResult> /*results*/) {
            // rpc_* routines handle state transitions themselves
            // (sync: DoAction 'complete', async: callback completes later)
            delete_report(id);
        },
        [this, id](std::string_view error) {
            if (in_progress(id))
                do_fail(id, std::string(error));
            else
                delete_report(id);
        });

    // Store query handle for cancel support
    auto it = reports_.find(id);
    if (it != reports_.end())
        it->second.query_id = qid;
}

// --- do_complete -------------------------------------------------------------

void ReportServer::do_complete(const std::string& id)
{
    logger_->debug("ReportServer: report {} complete", id);

    execute_action(id, "complete",
        [this, id](std::vector<PgResult> /*results*/) {
            delete_report(id);
        });
}

// --- do_abort ----------------------------------------------------------------

void ReportServer::do_abort(const std::string& id)
{
    logger_->notice("ReportServer: aborting report {}", id);

    // Cancel running SQL body if any (PQcancel → PostgreSQL)
    auto it = reports_.find(id);
    if (it != reports_.end() && it->second.query_id != 0)
        pool_->cancel(it->second.query_id);

    // Remove from tracking immediately — canceled query results will be discarded
    delete_report(id);

    execute_action(id, "abort",
        [](std::vector<PgResult> /*results*/) {
            // report already removed from reports_
        });
}

// --- do_fail -----------------------------------------------------------------
//
// Mark report as failed + store error as object label.
//

void ReportServer::do_fail(const std::string& id, const std::string& error)
{
    logger_->error("ReportServer: report {} failed: {}", id, error);

    if (!bot_->valid()) {
        delete_report(id);
        return;
    }

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_object_action({}::uuid, {});\n"
        "SELECT * FROM api.set_object_label({}::uuid, {})",
        pq_quote_literal(bot_->session()),
        pq_quote_literal(id), pq_quote_literal("fail"),
        pq_quote_literal(id), pq_quote_literal(error));

    pool_->execute(sql,
        [this, id](std::vector<PgResult> /*results*/) {
            delete_report(id);
        },
        [this, id](std::string_view err) {
            logger_->error("ReportServer: do_fail SQL error for {}: {}", id, err);
            delete_report(id);
        });
}

// --- execute_action ----------------------------------------------------------

void ReportServer::execute_action(const std::string& id, std::string_view action,
                                  PgQuery::ResultHandler on_result)
{
    bot_->execute_action(id, action, std::move(on_result),
        [this, id, act = std::string(action)](std::string_view error) {
            logger_->error("ReportServer: action '{}' failed for {}: {}", act, id, error);
            delete_report(id);
            on_fatal(std::string(error));
        });
}

// --- delete_report / in_progress ---------------------------------------------

void ReportServer::delete_report(const std::string& id)
{
    reports_.erase(id);
}

bool ReportServer::in_progress(const std::string& id) const
{
    return reports_.count(id) > 0;
}

// --- on_fatal ----------------------------------------------------------------
//
// Catastrophic error -- pause for 10 seconds before retrying.
//

void ReportServer::on_fatal(const std::string& error)
{
    status_ = Status::stopped;
    next_check_ = std::chrono::system_clock::now() + std::chrono::seconds(10);
    logger_->error("ReportServer: fatal error, pausing 10s: {}", error);
}

} // namespace apostol

#endif // WITH_POSTGRESQL
