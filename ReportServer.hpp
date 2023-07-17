/*++

Program name:

  Apostol CRM

Module Name:

  ReportServer.hpp

Notices:

  Module: Report Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#ifndef APOSTOL_REPORT_SERVER_HPP
#define APOSTOL_REPORT_SERVER_HPP
//----------------------------------------------------------------------------------------------------------------------

extern "C++" {

namespace Apostol {

    namespace Module {

        //--------------------------------------------------------------------------------------------------------------

        //-- CReportHandler --------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        class CReportHandler: public CQueueHandler {
        private:

            CString m_Session;
            CString m_ReportId;

            CJSON m_Payload;

        public:

            CReportHandler(CQueueCollection *ACollection, const CString &Data, COnQueueHandlerEvent && Handler);

            const CString &Session() const { return m_Session; }
            const CString &ReportId() const { return m_ReportId; }

            const CJSON &Payload() const { return m_Payload; }

        };

        //--------------------------------------------------------------------------------------------------------------

        //-- CReportServer ---------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        class CReportServer: public CQueueCollection, public CApostolModule {
        private:

            CProcessStatus m_Status;

            CStringList m_Sessions;
            CStringList m_Reports;

            CString m_Conf;
            CString m_Agent;
            CString m_Host;

            CDateTime m_CheckDate;
            CDateTime m_AuthDate;

            void InitMethods() override;

            void InitListen();
            void CheckListen();

            void Authentication();
            void SignOut(const CString &Session);

            int IndexOfReports(const CString &Id);
            int AddReport(const CString &Id);
            int DeleteReport(const CString &Id);

            void EnumReportReady(const CString &Session, const CPQueryResult &List);
            void CheckReportReady();

        protected:

            void DoFatal(const Delphi::Exception::Exception &E);
            void DoError(const Delphi::Exception::Exception &E);

            void DoStart(const CString &Session, const CString &Id);
            void DoComplete(const CString &Session, const CString &Id);
            void DoAbort(const CString &Session, const CString &Id);
            void DoCancel(const CString &Session, const CString &Id);
            void DoFail(const CString &Session, const CString &Id, const CString &Error);

            void DoReport(CQueueHandler *AHandler);

            void DoPostgresNotify(CPQConnection *AConnection, PGnotify *ANotify) override;

            void DoPostgresQueryExecuted(CPQPollQuery *APollQuery) override;
            void DoPostgresQueryException(CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) override;

        public:

            explicit CReportServer(CModuleProcess *AProcess);

            ~CReportServer() override = default;

            static class CReportServer *CreateModule(CModuleProcess *AProcess) {
                return new CReportServer(AProcess);
            }

            CPQPollQuery *GetQuery(CPollConnection *AConnection) override;

            void Heartbeat(CDateTime Now) override;
            void UnloadQueue() override;
            bool Enabled() override;
            bool CheckLocation(const CLocation &Location) override;

            void Reload();

        };
    }
}

using namespace Apostol::Module;
}
#endif //APOSTOL_REPORT_SERVER_HPP
