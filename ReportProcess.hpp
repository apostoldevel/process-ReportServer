/*++

Program name:

  Apostol CRM

Module Name:

  ReportProcess.hpp

Notices:

  Process: Report Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#ifndef APOSTOL_REPORT_PROCESS_HPP
#define APOSTOL_REPORT_PROCESS_HPP
//----------------------------------------------------------------------------------------------------------------------

#include "ReportServer/ReportServer.hpp"
//----------------------------------------------------------------------------------------------------------------------

extern "C++" {

namespace Apostol {

    namespace Processes {

        //--------------------------------------------------------------------------------------------------------------

        //-- CReportProcess --------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        class CReportProcess: public CApplicationProcess, public CModuleProcess {
            typedef CApplicationProcess inherited;

        private:

            CReportServer m_Report { this };

            size_t m_MaxMessagesQueue;

            void BeforeRun() override;
            void AfterRun() override;

            void DoHeartbeat(CDateTime Datetime);

        protected:

            void DoTimer(CPollEventHandler *AHandler) override;
            bool DoExecute(CTCPConnection *AConnection) override;

        public:

            explicit CReportProcess(CCustomProcess* AParent, CApplication *AApplication);

            ~CReportProcess() override = default;

            static class CReportProcess *CreateProcess(CCustomProcess *AParent, CApplication *AApplication) {
                return new CReportProcess(AParent, AApplication);
            }

            void Run() override;
            void Reload() override;

        };
        //--------------------------------------------------------------------------------------------------------------

    }
}

using namespace Apostol::Processes;
}
#endif //APOSTOL_REPORT_PROCESS_HPP
