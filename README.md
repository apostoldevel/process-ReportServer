[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru-RU.md)

Report Server
-
**ReportServer** is a process for [Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm].

Description
-
**ReportServer** executes pre-defined database reports asynchronously. A report is a PL/pgSQL routine registered in the report queue; the process picks it up, runs it as a PostgreSQL query, and updates the report's state when done.

The process runs independently inside the Apostol master process, sharing the same `epoll`-based event loop — no threads, no blocking I/O.

Architecture
-
`CReportProcess` is the OS-level process wrapper. It embeds `CReportServer`, which inherits from both `CQueueCollection` and `CApostolModule` and contains the actual report execution logic.

How it works
-
1. Authenticates via OAuth2 `client_credentials` as `apibot`; re-authenticates every 24 hours.
2. Subscribes to the PostgreSQL `report` notify channel via `LISTEN report`.
3. Polls `api.report_ready('enabled')` every minute as a fallback.
4. For each report in `progress` state, calls `api.execute_report_ready(id)` to run it.
5. On completion — `api.execute_object_action(id, 'complete')`.
6. On cancellation — cancels the active PQ query, then `api.execute_object_action(id, 'abort')`.
7. On error — `api.execute_object_action(id, 'fail')`.

The NOTIFY payload format on the `report` channel:

```json
{"session": "...", "id": "<report-uuid>"}
```

Report lifecycle
-

| State | Action |
|-------|--------|
| `progress` | Run `api.execute_report_ready(id)` |
| _(query done)_ | `execute_object_action('complete')` |
| `canceled` _(while running)_ | Cancel PQ query → `execute_object_action('abort')` |
| _(query error)_ | `execute_object_action('fail')` |

Database module
-
ReportServer is tightly coupled to the **`report`** module of [db-platform](https://github.com/apostoldevel/db-platform) (`db/sql/platform/report/`).

A report consists of four parts:

```
Report
  ├── Tree      — section hierarchy
  ├── Form      — user input definition (rfc_* function)
  ├── Routine   — generation code (rpc_* functions, ordered by sequence)
  └── Ready     — output document (db.report_ready), inherits Document lifecycle
```

Key database objects:

| Object | Purpose |
|--------|---------|
| `db.report_ready` | Output document that drives execution; state machine triggers `NOTIFY 'report'` on state change |
| `api.report_ready(state)` | Returns output documents in the given state (e.g. `'enabled'`) |
| `api.execute_report_ready(id)` | Runs the report's `rpc_*` routine(s) and produces output |
| `api.execute_object_action(id, action)` | State transitions: `'complete'`, `'abort'`, `'cancel'`, `'fail'` |
| `api.get_report_ready(id)` | Fetches report_ready record with current state |

Configuration
-
```ini
[module/ReportServer]
enable=true
```

> **Note:** The config section is `module/ReportServer`, not `process/ReportServer`. This is because the embedded `CReportServer` module reads its `enable` flag from the `module/` namespace.

Installation
-
Follow the build and installation instructions for [Apostol](https://github.com/apostoldevel/apostol#build-and-installation).

[^crm]: **Apostol CRM** is an abstract term, not a standalone product. It refers to any project that uses both the [Apostol](https://github.com/apostoldevel/apostol) C++ framework and [db-platform](https://github.com/apostoldevel/db-platform) together through purpose-built modules and processes. Each framework can be used independently; combined, they form a full-stack backend platform.
