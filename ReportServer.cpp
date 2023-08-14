/*++

Program name:

  Apostol CRM

Module Name:

  ReportServer.cpp

Notices:

  Module: Report Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

//----------------------------------------------------------------------------------------------------------------------

#include "Core.hpp"
#include "ReportServer.hpp"
//----------------------------------------------------------------------------------------------------------------------

#define API_BOT_USERNAME "apibot"

#define QUERY_INDEX_AUTH     0
#define QUERY_INDEX_DATA     1

#define SLEEP_SECOND_AFTER_ERROR 10

#define PG_CONFIG_NAME "helper"
#define PG_LISTEN_NAME "report"
//----------------------------------------------------------------------------------------------------------------------

extern "C++" {

namespace Apostol {

    namespace BackEnd {

        namespace api {

            void report_ready(CStringList &SQL, const CString &State) {
                SQL.Add(CString().Format("SELECT * FROM api.report_ready(%s::text) ORDER BY created;", PQQuoteLiteral(State).c_str()));
            }
            //----------------------------------------------------------------------------------------------------------

            void get_report_ready(CStringList &SQL, const CString &Id) {
                SQL.Add(CString().MaxFormatSize(256 + Id.Size())
                                .Format("SELECT id, statecode FROM api.get_report_ready(%s::uuid)",
                                        PQQuoteLiteral(Id).c_str()
                                ));
            }
            //----------------------------------------------------------------------------------------------------------

            void execute_report_ready(CStringList &SQL, const CString &Id) {
                SQL.Add(CString().MaxFormatSize(256 + Id.Size())
                                .Format("SELECT * FROM api.execute_report_ready(%s::uuid)",
                                        PQQuoteLiteral(Id).c_str()
                                ));
            }
            //----------------------------------------------------------------------------------------------------------

        }
    }

    namespace Module {

        //--------------------------------------------------------------------------------------------------------------

        //-- CReportHandler --------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        CReportHandler::CReportHandler(CQueueCollection *ACollection, const CString &Data, COnQueueHandlerEvent && Handler):
                CQueueHandler(ACollection, static_cast<COnQueueHandlerEvent &&> (Handler)) {

            m_Payload = Data;

            m_Session = m_Payload["session"].AsString();
            m_ReportId = m_Payload["id"].AsString();
        }

        //--------------------------------------------------------------------------------------------------------------

        //-- CReportServer ---------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        CReportServer::CReportServer(CModuleProcess *AProcess): CQueueCollection(Config()->PostgresPollMin()),
                CApostolModule(AProcess, "report server", "module/ReportServer") {

            m_Agent = CString().Format("Report Server (%s)", GApplication->Title().c_str());
            m_Host = CApostolModule::GetIPByHostName(CApostolModule::GetHostName());

            m_Conf = PG_CONFIG_NAME;

            m_CheckDate = 0;
            m_AuthDate = 0;

            m_Status = Process::psStopped;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::InitMethods() {

        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::InitListen() {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {
                try {
                    auto pResult = APollQuery->Results(0);

                    if (pResult->ExecStatus() != PGRES_COMMAND_OK) {
                        throw Delphi::Exception::EDBError(pResult->GetErrorMessage());
                    }

                    APollQuery->Connection()->Listeners().Add(PG_LISTEN_NAME);
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                    APollQuery->Connection()->OnNotify([this](auto && APollQuery, auto && ANotify) { DoPostgresNotify(APollQuery, ANotify); });
#else
                    APollQuery->Connection()->OnNotify(std::bind(&CReportServer::DoPostgresNotify, this, _1, _2));
#endif
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoFatal(E);
            };

            CStringList SQL;

            SQL.Add("LISTEN " PG_LISTEN_NAME ";");

            try {
                ExecSQL(SQL, nullptr, OnExecuted, OnException);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::CheckListen() {
            if (!PQClient(PG_CONFIG_NAME).CheckListen(PG_LISTEN_NAME))
                InitListen();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::Authentication() {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {

                CPQueryResults pqResults;

                CStringList SQL;

                try {
                    CApostolModule::QueryToResults(APollQuery, pqResults);

                    const auto &login = pqResults[0];
                    const auto &sessions = pqResults[1];

                    const auto &session = login.First()["session"];

                    m_Sessions.Clear();
                    for (int i = 0; i < sessions.Count(); ++i) {
                        m_Sessions.Add(sessions[i]["get_sessions"]);
                    }

                    m_AuthDate = Now() + (CDateTime) 24 / HoursPerDay;
                    m_Status = psRunning;

                    SignOut(session);
                } catch (Delphi::Exception::Exception &E) {
                    DoFatal(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoFatal(E);
            };

            const auto &caProviders = Server().Providers();
            const auto &caProvider = caProviders.DefaultValue();

            const auto &clientId = caProvider.ClientId(SERVICE_APPLICATION_NAME);
            const auto &clientSecret = caProvider.Secret(SERVICE_APPLICATION_NAME);

            CStringList SQL;

            api::login(SQL, clientId, clientSecret, m_Agent, m_Host);
            api::get_sessions(SQL, API_BOT_USERNAME, m_Agent, m_Host);

            try {
                ExecSQL(SQL, nullptr, OnExecuted, OnException);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::SignOut(const CString &Session) {
            CStringList SQL;

            api::signout(SQL, Session);

            try {
                ExecSQL(SQL);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::EnumReportReady(const CString &Session, const CPQueryResult &List) {
            int index;
            CString Error;

            for (int row = 0; row < List.Count(); ++row) {
                const auto &rec = List[row];

                const auto &id = rec["id"];
                const auto &state_code = rec["statecode"];

                index = m_Reports.IndexOf(id);
                if (index != -1) {
                    if (state_code == "canceled") {
                        auto pQuery = dynamic_cast<CPQQuery *> (m_Reports.Objects(index));
                        if (pQuery != nullptr) {
                            if (pQuery->CancelQuery(Error)) {
                                DoAbort(Session, id);
                            } else {
                                DoFail(Session, id, Error);
                            }
                        }
                    }
                } else {
                    if (state_code == "progress") {
                        DoStart(Session, id);
                    }
                }
            }
        }

        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::CheckReportReady() {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {

                CPQueryResults pqResults;
                CStringList SQL;

                const auto &session = APollQuery->Data()["session"];

                try {
                    CApostolModule::QueryToResults(APollQuery, pqResults);

                    const auto &authorize = pqResults[QUERY_INDEX_AUTH].First();

                    if (authorize["authorized"] != "t")
                        throw Delphi::Exception::ExceptionFrm("Authorization failed: %s", authorize["message"].c_str());

                    EnumReportReady(session, pqResults[QUERY_INDEX_DATA]);
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoFatal(E);
            };

            for (int i = 0; i < m_Sessions.Count(); ++i) {
                const auto &session = m_Sessions[i];

                CStringList SQL;

                api::authorize(SQL, session);
                api::report_ready(SQL, "enabled");

                try {
                    auto pQuery = ExecSQL(SQL, nullptr, OnExecuted, OnException);
                    pQuery->Data().AddPair("session", session);
                } catch (Delphi::Exception::Exception &E) {
                    DoFatal(E);
                }
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        int CReportServer::IndexOfReports(const CString &Id) {
            return m_Reports.IndexOf(Id);
        }
        //--------------------------------------------------------------------------------------------------------------

        int CReportServer::AddReport(const CString &Id) {
            const auto index = IndexOfReports(Id);
            if (index == -1)
                return m_Reports.Add(Id);
            return index;
        }
        //--------------------------------------------------------------------------------------------------------------

        int CReportServer::DeleteReport(const CString &Id) {
            const auto index = IndexOfReports(Id);
            if (index != -1)
                m_Reports.Delete(index);
            return index;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoStart(const CString &Session, const CString &Id) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {

                const auto &session = APollQuery->Data()["session"];
                const auto &id = APollQuery->Data()["id"];

                CPQResult *pResult;
                try {
                    for (int i = 0; i < APollQuery->Count(); i++) {
                        pResult = APollQuery->Results(i);

                        if (pResult->ExecStatus() != PGRES_TUPLES_OK)
                            throw Delphi::Exception::EDBError(pResult->GetErrorMessage());
                    }
                } catch (Delphi::Exception::Exception &E) {
                    DeleteReport(id);
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                DoFatal(E);
            };

            CStringList SQL;

            api::authorize(SQL, Session);
            api::execute_report_ready(SQL, Id);

            Log()->Message("[%s] Report started.", Id.c_str());

            try {
                auto pQuery = ExecSQL(SQL, nullptr, OnExecuted, OnException);

                pQuery->Data().AddPair("session", Session);
                pQuery->Data().AddPair("id", Id);

                m_Reports.AddObject(Id, (CPQQuery *) pQuery);
            } catch (Delphi::Exception::Exception &E) {
                DeleteReport(Id);
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoComplete(const CString &Session, const CString &Id) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                Log()->Message("[%s] Report completed.", id.c_str());
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                DoError(E);
            };

            CStringList SQL;

            api::authorize(SQL, Session);
            api::execute_object_action(SQL, Id, "complete");

            try {
                auto pQuery = ExecSQL(SQL, nullptr, OnExecuted, OnException);
                pQuery->Data().AddPair("id", Id);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoAbort(const CString &Session, const CString &Id) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                Log()->Message("[%s] Report aborted.", id.c_str());
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                DoError(E);
            };

            CStringList SQL;

            api::authorize(SQL, Session);
            api::execute_object_action(SQL, Id, "abort");

            try {
                auto pQuery = ExecSQL(SQL, nullptr, OnExecuted, OnException);
                pQuery->Data().AddPair("id", Id);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoCancel(const CString &Session, const CString &Id) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                Log()->Message("[%s] Report canceled.", id.c_str());
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                DoError(E);
            };

            CStringList SQL;

            api::authorize(SQL, Session);
            api::execute_object_action(SQL, Id, "cancel");

            try {
                auto pQuery = ExecSQL(SQL, nullptr, OnExecuted, OnException);
                pQuery->Data().AddPair("id", Id);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoFail(const CString &Session, const CString &Id, const CString &Error) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                Log()->Message("[%s] Report failed.", id.c_str());
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                const auto &id = APollQuery->Data()["id"];
                DeleteReport(id);
                DoError(E);
            };

            CStringList SQL;

            api::authorize(SQL, Session);
            api::execute_object_action(SQL, Id, "fail");
            api::set_object_label(SQL, Id, Error);

            try {
                auto pQuery = ExecSQL(SQL, nullptr, OnExecuted, OnException);
                pQuery->Data().AddPair("id", Id);
            } catch (Delphi::Exception::Exception &E) {
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoFatal(const Delphi::Exception::Exception &E) {
            m_AuthDate = Now() + (CDateTime) SLEEP_SECOND_AFTER_ERROR / SecsPerDay; // 10 sec;
            m_CheckDate = m_AuthDate;

            m_Status = Process::psStopped;

            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
            Log()->Notice("[ReportServer] Continue after %d seconds", SLEEP_SECOND_AFTER_ERROR);
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoError(const Delphi::Exception::Exception &E) {
            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoReport(CQueueHandler *AHandler) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {

                CPQueryResults pqResults;
                CStringList SQL;

                auto pHandler = dynamic_cast<CReportHandler *> (APollQuery->Binding());

                if (pHandler == nullptr) {
                    return;
                }

                try {
                    CApostolModule::QueryToResults(APollQuery, pqResults);
                    const auto &caReports = pqResults[QUERY_INDEX_DATA];
                    if (caReports.Count() > 0) {
                        EnumReportReady(pHandler->Session(), caReports);
                    }
                } catch (Delphi::Exception::Exception &E) {
                    Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
                }

                DeleteHandler(pHandler);
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                auto pHandler = dynamic_cast<CReportHandler *> (APollQuery->Binding());
                DeleteHandler(pHandler);
                DoFatal(E);
            };

            auto pHandler = dynamic_cast<CReportHandler *> (AHandler);

            if (IndexOfReports(pHandler->ReportId()) >= 0) {
                Log()->Error(APP_LOG_WARN, 0, "[%s] Report already in progress.", pHandler->ReportId().c_str());
                DeleteHandler(AHandler);
                return;
            }

            CStringList SQL;

            api::authorize(SQL, pHandler->Session());
            api::get_report_ready(SQL, pHandler->ReportId());

            try {
                ExecSQL(SQL, AHandler, OnExecuted, OnException);
                AHandler->Allow(false);
                IncProgress();
            } catch (Delphi::Exception::Exception &E) {
                DeleteHandler(AHandler);
                DoFatal(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoPostgresNotify(CPQConnection *AConnection, PGnotify *ANotify) {
            DebugNotify(AConnection, ANotify);

            if (CompareString(ANotify->relname, PG_LISTEN_NAME) == 0) {
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                new CReportHandler(this, ANotify->extra, [this](auto &&Handler) { DoReport(Handler); });
#else
                new CReportHandler(this, ANotify->extra, std::bind(&CReportServer::DoReport, this, _1));
#endif
                UnloadQueue();
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoPostgresQueryExecuted(CPQPollQuery *APollQuery) {
            CPQResult *pResult;

            try {
                for (int i = 0; i < APollQuery->Count(); i++) {
                    pResult = APollQuery->Results(i);
                    if (pResult->ExecStatus() != PGRES_TUPLES_OK)
                        throw Delphi::Exception::EDBError(pResult->GetErrorMessage());
                }
            } catch (Delphi::Exception::Exception &E) {
                DoError(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::DoPostgresQueryException(CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
            DoFatal(E);
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::UnloadQueue() {
            const auto index = m_Queue.IndexOf(this);
            if (index != -1) {
                const auto queue = m_Queue[index];
                for (int i = 0; i < queue->Count(); ++i) {
                    auto pHandler = (CReportHandler *) queue->Item(i);
                    if (pHandler != nullptr) {
                        pHandler->Handler();
                        if (m_Progress >= m_MaxQueue)
                            break;
                    }
                }
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        CPQPollQuery *CReportServer::GetQuery(CPollConnection *AConnection) {
            CPQPollQuery *pQuery = m_pModuleProcess->GetQuery(AConnection, m_Conf);

            if (Assigned(pQuery)) {
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                pQuery->OnPollExecuted([this](auto && APollQuery) { DoPostgresQueryExecuted(APollQuery); });
                pQuery->OnException([this](auto && APollQuery, auto && AException) { DoPostgresQueryException(APollQuery, AException); });
#else
                pQuery->OnPollExecuted(std::bind(&CReportServer::DoPostgresQueryExecuted, this, _1));
                pQuery->OnException(std::bind(&CReportServer::DoPostgresQueryException, this, _1, _2));
#endif
            }

            return pQuery;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::Heartbeat(CDateTime Now) {
            if ((Now >= m_AuthDate)) {
                m_AuthDate = Now + (CDateTime) 5 / SecsPerDay; // 5 sec
                Authentication();
            }

            if (m_Status == Process::psRunning) {
                UnloadQueue();
                if ((Now >= m_CheckDate)) {
                    m_CheckDate = Now + (CDateTime) 1 / MinsPerDay; // 1 min
                    CheckListen();
                    if (m_Queue.IndexOf(this) == -1) {
                        CheckReportReady();
                    }
                }
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportServer::Reload() {
            m_AuthDate = 0;
            m_CheckDate = 0;

            m_Status = Process::psStopped;

            Log()->Notice("[ReportServer] Reloading...");
        }
        //--------------------------------------------------------------------------------------------------------------

        bool CReportServer::Enabled() {
            if (m_ModuleStatus == msUnknown)
                m_ModuleStatus = Config()->IniFile().ReadBool(SectionName().c_str(), "enable", true) ? msEnabled : msDisabled;
            return m_ModuleStatus == msEnabled;
        }
        //--------------------------------------------------------------------------------------------------------------

        bool CReportServer::CheckLocation(const CLocation &Location) {
            return false;
        }
    }
}
}