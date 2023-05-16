/*++

Program name:

  Apostol CRM

Module Name:

  ReportProcess.cpp

Notices:

  Process: Report Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#include "Core.hpp"
#include "ReportProcess.hpp"
//----------------------------------------------------------------------------------------------------------------------

extern "C++" {

namespace Apostol {

    namespace Processes {

        //--------------------------------------------------------------------------------------------------------------

        //-- CReportProcess --------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        CReportProcess::CReportProcess(CCustomProcess *AParent, CApplication *AApplication):
                inherited(AParent, AApplication, ptCustom, "report server process") {

            m_MaxMessagesQueue = Config()->PostgresPollMin();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportProcess::BeforeRun() {
            Application()->Header(Application()->Name() + ": report server");

            Log()->Debug(APP_LOG_DEBUG_CORE, MSG_PROCESS_START, GetProcessName(), Application()->Header().c_str());

            InitSignals();

            Reload();

            SetUser(Config()->User(), Config()->Group());

            InitializePQClients(Application()->Title(), 1, m_MaxMessagesQueue);

            SigProcMask(SIG_UNBLOCK);

            SetTimerInterval(1000);
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportProcess::AfterRun() {
            CApplicationProcess::AfterRun();
            PQClientsStop();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportProcess::Run() {
            auto &PQClient = PQClientStart(_T("helper"));

            while (!sig_exiting) {

                Log()->Debug(APP_LOG_DEBUG_EVENT, _T("report server process cycle"));

                try {
                    PQClient.Wait();
                } catch (Delphi::Exception::Exception &E) {
                    Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
                }

                if (sig_terminate || sig_quit) {
                    if (sig_quit) {
                        sig_quit = 0;
                        Log()->Debug(APP_LOG_DEBUG_EVENT, _T("gracefully shutting down"));
                        Application()->Header(_T("report server process is shutting down"));
                    }

                    if (!sig_exiting) {
                        sig_exiting = 1;
                    }
                }

                if (sig_reconfigure) {
                    sig_reconfigure = 0;
                    Log()->Debug(APP_LOG_DEBUG_EVENT, _T("reconfiguring"));

                    Reload();
                }

                if (sig_reopen) {
                    sig_reopen = 0;
                    Log()->Debug(APP_LOG_DEBUG_EVENT, _T("reopening logs"));
                }
            }

            Log()->Debug(APP_LOG_DEBUG_EVENT, _T("stop report server process"));
        }
        //--------------------------------------------------------------------------------------------------------------

        bool CReportProcess::DoExecute(CTCPConnection *AConnection) {
            return CModuleProcess::DoExecute(AConnection);
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportProcess::Reload() {
            CServerProcess::Reload();
            m_Report.Reload();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportProcess::DoTimer(CPollEventHandler *AHandler) {
            uint64_t exp;

            auto pTimer = dynamic_cast<CEPollTimer *> (AHandler->Binding());
            pTimer->Read(&exp, sizeof(uint64_t));

            try {
                DoHeartbeat(AHandler->TimeStamp());
                CModuleProcess::HeartbeatModules(AHandler->TimeStamp());
            } catch (Delphi::Exception::Exception &E) {
                DoServerEventHandlerException(AHandler, E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CReportProcess::DoHeartbeat(CDateTime Datetime) {

        }
        //--------------------------------------------------------------------------------------------------------------

    }
}

}
