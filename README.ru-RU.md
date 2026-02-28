[![en](https://img.shields.io/badge/lang-en-green.svg)](README.md)

Сервер отчётов
-

**Процесс** для [Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm].

Описание
-

**Сервер отчётов** — фоновый процесс-модуль для фреймворка [Апостол](https://github.com/apostoldevel/apostol). Запускается как отдельный форкнутый процесс и выполняет заранее подготовленные отчёты из базы данных в асинхронном режиме. Отчёт — это PL/pgSQL-процедура, зарегистрированная в очереди отчётов; процесс извлекает её, выполняет как запрос PostgreSQL и обновляет состояние отчёта по завершении.

Основные характеристики:

* Написан на C++20 с использованием асинхронной неблокирующей модели ввода-вывода на базе **epoll** API.
* Подключается к **PostgreSQL** через библиотеку `libpq`, используя роль `apibot` (пул соединений helper).
* Аутентифицируется через OAuth2 `client_credentials` с помощью `BotSession` — повторная аутентификация каждые 24 часа.
* **NOTIFY-driven**: подписывается на PostgreSQL-канал `LISTEN report` для немедленной обработки.
* **Polling-fallback**: каждую минуту проверяет `api.report_ready('enabled')` для обнаружения пропущенных уведомлений.
* Обрабатывает ошибки: для неуспешных отчётов сохраняется сообщение об ошибке; при критических ошибках сервер приостанавливается на 10 секунд.

### Архитектура

Сервер отчётов следует паттерну **ProcessModule**, введённому в apostol.v2:

```
Application
  └── ModuleProcess (generic-оболочка процесса: сигналы, EventLoop, PgPool)
        └── ReportServer (ProcessModule: только бизнес-логика)
```

Жизненный цикл процесса (обработка сигналов, crash recovery, настройка PgPool, таймер heartbeat) управляется generic-оболочкой `ModuleProcess`. `ReportServer` содержит только логику выполнения отчётов.

### Как это работает

```
heartbeat (1 сек)
  └── BotSession::refresh_if_needed()
  └── если аутентифицирован:
        └── process_notify_queue()  — немедленная обработка NOTIFY
        └── если now >= next_check_ (1 мин):
              └── check_reports()   — polling-fallback
                    └── api.authorize(session)
                    └── api.report_ready('enabled') ORDER BY created
                    └── enum_reports():
                          для каждого отчёта:
                            progress && !in_progress → do_start(id)
                              └── api.execute_report_ready(id)
                              └── успех   → do_complete() → action 'complete'
                              └── ошибка  → do_fail()  → action 'fail' + set_object_label
                            canceled && in_progress → do_abort(id)
                              └── api.execute_object_action(id, 'abort')

NOTIFY "report" → on_notify(payload):
  парсинг JSON → id
  если !in_progress → pending_reports_.push(id)
  → обрабатывается на следующем heartbeat через do_check(id)
    └── api.get_report_ready(id)
    └── если statecode == "progress" → do_start(id)
```

### Формат NOTIFY payload

```json
{"session": "...", "id": "<report-uuid>"}
```

### Машина состояний отчёта (db-platform)

```
created ──enable──► enabled ──execute──► progress ──complete──► completed
                                                   ──fail────► failed
                                                   ──cancel──► canceled ──abort──► aborted
```

Модуль базы данных
-

ReportServer тесно связан с модулем **`report`** платформы [db-platform](https://github.com/apostoldevel/db-platform) (`db/sql/platform/report/`).

Отчёт состоит из четырёх частей:

```
Отчёт
  ├── Tree (дерево)  — иерархия разделов
  ├── Form (форма)   — определение ввода данных (функции rfc_*)
  ├── Routine        — код генерации (функции rpc_*, упорядоченные по sequence)
  └── Ready (готово) — выходной документ (db.report_ready), наследует жизненный цикл Document
```

Ключевые объекты базы данных:

| Объект | Назначение |
|--------|-----------|
| `db.report_ready` | Выходной документ, управляющий выполнением; конечный автомат отправляет `NOTIFY 'report'` при смене состояния |
| `api.report_ready(state)` | Возвращает выходные документы в заданном состоянии (например, `'enabled'`) |
| `api.execute_report_ready(id)` | Запускает подпрограмму(ы) `rpc_*` отчёта и формирует вывод |
| `api.execute_object_action(id, action)` | Переходы состояний: `'complete'`, `'abort'`, `'cancel'`, `'fail'` |
| `api.get_report_ready(id)` | Получает запись report_ready с текущим состоянием |

Конфигурация
-

В конфигурационном файле приложения (`conf/apostol.json`):

```json
{
  "module": {
    "ReportServer": {
      "enable": true,
      "heartbeat": 60000
    }
  }
}
```

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `enable` | bool | `false` | Включить/отключить процесс |
| `heartbeat` | int | `60000` | Интервал проверки отчётов в миллисекундах |

Также необходимы:
* Строка подключения `postgres.helper` в конфигурации (используется для пула соединений `apibot`)
* Учётные данные OAuth2 `service` в файле `conf/oauth2/default.json`

Установка
-

Следуйте указаниям по сборке и установке [Апостол](https://github.com/apostoldevel/apostol#building-and-installation).

[^crm]: **Apostol CRM** — абстрактный термин, а не самостоятельный продукт. Он обозначает любой проект, в котором совместно используются фреймворк [Apostol](https://github.com/apostoldevel/apostol) (C++) и [db-platform](https://github.com/apostoldevel/db-platform) через специально разработанные модули и процессы. Каждый фреймворк можно использовать независимо; вместе они образуют полноценную backend-платформу.
