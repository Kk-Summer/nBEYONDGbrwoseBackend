#include <QtHttpServer>
#include <QJsonDocument>
#include <QSettings>
#include <QTextCodec>
#include "httpservice.h"
#include "databasequery.h"

HttpService::HttpService(QSqlDatabase databaseConnection) :
    databaseQuery(DatabaseQuery(databaseConnection))
{
    this->loadHttpServiceConfig();
    this->initUrlRouting();
}

bool HttpService::startListening()
{
    int portResult =
            snowHttpServer.listen(QHostAddress::Any, this->listenPort);

    if(portResult != this->listenPort)
    {
        QString errorMsg = QString("[Error] Could not run on http://0.0.0.0:%1 Code:%2")
                .arg(this->listenPort, portResult);
        qDebug() << errorMsg.toUtf8().data();
        return false;
    }
    else
    {
        QString successMsg = QString("[Info] Running on http://0.0.0.0:%1/ Success!")
                .arg(this->listenPort);
        qDebug() << successMsg.toUtf8().data();
        return true;
    }
}

void HttpService::loadHttpServiceConfig()
{
    QSettings snowHttpSettings("./config.ini", QSettings::IniFormat);
    snowHttpSettings.setIniCodec(QTextCodec::codecForName("UTF-8"));
    snowHttpSettings.beginGroup("General");

    uint listenPort = snowHttpSettings.value(
        QString("listenPort"),
        12080
    ).toUInt();
    this->listenPort = quint16(listenPort);

    snowHttpSettings.endGroup();
}

// Encapsulate the process of writing data,
// Add request headers to resolve 'CORS (Cross-Origin Resource Sharing)' issue on modern browsers
void HttpService::writeResponseData(QHttpServerResponder &responder, const QJsonDocument &document, QHttpServerResponder::StatusCode status)
{
    std::initializer_list<std::pair<QByteArray, QByteArray>> headerList =
    {
        std::pair<QByteArray, QByteArray>(
            QByteArrayLiteral("Access-Control-Allow-Origin"),
            QByteArrayLiteral("*")
        ),
        std::pair<QByteArray, QByteArray>(
            QByteArrayLiteral("Access-Control-Allow-Methods"),
            QByteArrayLiteral("GET, POST")
        ),
        std::pair<QByteArray, QByteArray>(
            QByteArrayLiteral("Access-Control-Allow-Headers"),
            QByteArrayLiteral("X-Requested-With")
        )
    };
    responder.write(document, headerList);
}

void HttpService::initUrlRouting()
{
    this->snowHttpServer.route("/", [](){
        return "Welcome to BeyondGBrowse web interface!";
    });

    // http://localhost:12080/1/ref/chr1/149813549..149813576
    // ????????????????????????????????????????????????
    // ????????? ?????????ID???????????????????????????????????????????????????
    // ??????Json?????????????????????????????????????????????arrMSScanMassArray???????????????arrMSScanPeakAundance???????????????_start???????????????end????????????????????????sequence????????????strand???scanId???uniprot_id
    // 
    // 2022-12-4 wz add ???????????????????????????Fragmentation (CID or ETD) 
    this->snowHttpServer.route("/<arg>/ref/<arg>/<arg>", [this](quint16 datasetId, QString proteinName, QString position, QHttpServerResponder &&responder){
        QJsonArray recordArray;
        try {
            recordArray = this->queryProteinByReferenceSequenceRegion(datasetId, proteinName, position);

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/ref/%1/%2 :")
                    .arg(proteinName, position) + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        this->writeResponseData(responder, QJsonDocument(recordArray));
    });

    // http://localhost:12080/1/locate/H32_HUMAN
    // ??????????????????????????????&????????????
    // ????????? ?????????ID???EnsembleId???UniprotId??????????????????????????????
    // ??????Json?????????Eg:
    // [
    //    {
    //       "_start":"149813271",
    //       "end":"149813681",
    //       "name":"chr1"
    //    },
    //    {
    //       "_start":"149839538",
    //       "end":"149841193",
    //       "name":"chr1"
    //    },
    //    {
    //       "_start":"149852619",
    //       "end":"149854274",
    //       "name":"chr1"
    //    }
    // ]

    this->snowHttpServer.route("/<arg>/locate/<arg>", [this](quint16 datasetId, QString proteinName, QHttpServerResponder &&responder){
        QJsonArray recordArray;
        try {
            recordArray = this->queryRegionByProteinId(datasetId, proteinName);

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/locate/%1 :")
                    .arg(proteinName) + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        this->writeResponseData(responder, QJsonDocument(recordArray));
    });

    // http://localhost:12080/1/annotation/query/Scan998/85..92
    // ?????????????????????????????????
    // ????????? ?????????ID?????????????????????/???????????????scanId??????????????????????????????
    // ??????Json?????????Eg:
    // [
    //    {
    //       "contents":"TEST",
    //       "name":"Scan998",
    //       "position":"89",
    //       "time":"2019-10-13T16:24:01.000"
    //    }
    // ]
    this->snowHttpServer.route("/<arg>/annotation/query/<arg>/<arg>", [this](quint16 datasetId, QString name, QString position, QHttpServerResponder &&responder){
        QJsonArray recordArray;
        try {
            QStringList posList = position.split("..");
            recordArray = this->queryAnnotationBySequenceRegion(datasetId, name, posList.at(0), posList.at(1));

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/annotation/query/%1/%2 :")
                    .arg(name, position) + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        this->writeResponseData(responder, QJsonDocument(recordArray));
    });

    // http://localhost:12080/annotation/insert
    // ???????????????????????????
    // ????????? ?????????ID?????????????????????/???????????????scanId????????????????????????????????????
    // ????????????{status: 'SUCCESS'}???????????????{status: 'FAIL'}
    //
    this->snowHttpServer.route("/annotation/insert", [this](const QHttpServerRequest &request, QHttpServerResponder &&responder){
        quint16 datasetId = request.query().queryItemValue("datasetId").toUInt();
        QString name = request.query().queryItemValue("refName");
        qint32 position = request.query().queryItemValue("position").toInt();
        QString time = request.query().queryItemValue("time", QUrl::FullyDecoded);
        QString authorUsername = request.query().queryItemValue("author");
        QString remoteAddress = request.remoteAddress().toString();
        QString contents = QString::fromUtf8(request.body());

        bool isInsertSuccess;
        try {
            isInsertSuccess = this->insertSequenceAnnotationAtSpecificPosition(
                        datasetId, 0, name, position, time, contents, authorUsername, remoteAddress);

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/annotation/insert/%1/%2/%3/... :")
                    .arg(name, QString::number(position), time) + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        if(isInsertSuccess)
        {
            QJsonObject jsonObjectToResponse({ QPair<QString, QJsonValue>(QString("status"), QString("SUCCESS")) });
            QJsonDocument jsonDocumentToResponse(jsonObjectToResponse);
            this->writeResponseData(responder, jsonDocumentToResponse);
        }
        else
        {
            QJsonObject jsonObjectToResponse({ QPair<QString, QJsonValue>(QString("status"), QString("FAIL")) });
            QJsonDocument jsonDocumentToResponse(jsonObjectToResponse);
            this->writeResponseData(responder, jsonDocumentToResponse);
        }
    });

    // http://localhost:12080/datasets
    // ?????????????????????
    // ????????? ???
    // ??????Json????????? ????????????????????????????????????id?????????name
    //
    this->snowHttpServer.route("/datasets", [this](QHttpServerResponder &&responder){
        QJsonArray recordArray;
        try {
            recordArray = this->queryDatasetsList();

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/datasets :")
                    + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        this->writeResponseData(responder, QJsonDocument(recordArray));
    });

    // http://localhost:12080/1/locate_autocomplete/H32
    // ???????????????xxx??????????????????ID
    // ????????? ?????????ID??? UniprotId
    // ??????Json?????????Eg:
    // [
    //     "H32_HUMAN",
    //     "H0YH32_HUMAN",
    //     "KLH32_HUMAN",
    //     "A0A0G2JH32_HUMAN"
    // ]
    this->snowHttpServer.route("/<arg>/locate_autocomplete/<arg>", [this](quint16 datasetId, QString proteinName, QHttpServerResponder &&responder){
        QJsonArray recordArray;
        try {
            recordArray = this->queryProteinIdListForAutoComplete(datasetId, proteinName);

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/locate_autocomplete/%1 :")
                    .arg(proteinName) + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        this->writeResponseData(responder, QJsonDocument(recordArray));
    });

    // http://localhost:12080/annotation/search
    // ????????????
    // ????????? ?????????ID?????????????????????/???????????????scanId????????????????????????????????????
    // ????????????????????????QJsonArray
    //
    this->snowHttpServer.route("/annotation/search", [this](const QHttpServerRequest &request, QHttpServerResponder &&responder){
        quint16 datasetId = request.query().queryItemValue("datasetId").toUInt();
        qint32 id = request.query().queryItemValue("id").toInt();
        QString authorUsername = request.query().queryItemValue("author");
        QString remoteAddress = request.query().queryItemValue("ipaddress");
        QString contents = QString::fromUtf8(request.body());

        QJsonArray recordArray;
        try {
            recordArray = this->searchAnnotation(datasetId, id, contents, authorUsername, remoteAddress);

        } catch (QString errorReason) {
            errorReason = "[Warning] " + QString("/annotation/search/%1/%2/%3/%4... :")
                    .arg(QString::number(datasetId), QString::number(id), authorUsername, remoteAddress) + errorReason;
            qDebug() << errorReason.toUtf8().data();
        }

        this->writeResponseData(responder, QJsonDocument(recordArray));
    });
}



QJsonArray HttpService::queryProteinByReferenceSequenceRegion(
            quint16 datasetId, QString proteinName, QString position )
{
    QStringList posList = position.split("..");
    if(posList.size() != 2)
    {
        throw QString("ERROR_QUERY_ARGUMENT_INVALID");
    }
    QJsonArray result = this->databaseQuery
            .queryProteinBySequenceRegion(
                datasetId, proteinName,posList.at(0),posList.at(1)
             );

    if(result.isEmpty())
    {
        throw QString("NOT_FOUND");
    }
    return result;
}

QJsonArray HttpService::queryRegionByProteinId(quint16 datasetId, QString proteinName)
{
    if(proteinName.isEmpty())
    {
        throw QString("ERROR_QUERY_ARGUMENT_INVALID");
    }
    QJsonArray result = this->databaseQuery
            .queryRegionByProteinId(datasetId, proteinName);

    if(result.isEmpty())
    {
        throw QString("NOT_FOUND");
    }
    return result;
}

QJsonArray HttpService::queryAnnotationBySequenceRegion(quint16 datasetId, QString name, QString posStart, QString posEnd)
{
    if(name.isEmpty() || posStart.isEmpty() || posEnd.isEmpty())
    {
        throw QString("ERROR_QUERY_ARGUMENT_INVALID");
    }
    QJsonArray result = this->databaseQuery
            .queryAnnotationBySequenceRegion(datasetId, name, posStart, posEnd);

    if(result.isEmpty())
    {
        throw QString("NOT_FOUND");
    }
    return result;
}

bool HttpService::insertSequenceAnnotationAtSpecificPosition(quint16 datasetId, qint32 id, QString name, qint32 position, QString time, QString contents, QString authorUsername, QString remoteAddress)
{
    if(name.isEmpty() || time.isEmpty() || contents.isEmpty())
    {
        throw QString("ERROR_QUERY_ARGUMENT_INVALID");
    }
    bool result = this->databaseQuery
            .insertSequenceAnnotationAtSpecificPosition(
                datasetId, 0, name, position, time, contents, authorUsername, remoteAddress
            );

    return result;
}

QJsonArray HttpService::queryDatasetsList()
{
    return this->databaseQuery.queryDatasetsList();
}

QJsonArray HttpService::queryProteinIdListForAutoComplete(quint16 datasetId, QString proteinName)
{
    if(proteinName.isEmpty())
    {
        throw QString("ERROR_QUERY_ARGUMENT_INVALID");
    }
    QJsonArray result = this->databaseQuery
            .queryProteinIdListForAutoComplete(datasetId, proteinName);

    if(result.isEmpty())
    {
        throw QString("NOT_FOUND");
    }
    return result;
}

QJsonArray HttpService::searchAnnotation(quint16 datasetId, qint32 id, QString contents, QString authorUsername, QString remoteAddress)
{
    if(
        (datasetId <= 0 || datasetId >= 5000) ||
        (
            id < 0 && contents.isEmpty() && authorUsername.isEmpty()
            && remoteAddress.isEmpty()
        )
    )
    {
        throw QString("ERROR_QUERY_ARGUMENT_INVALID");
    }
    QJsonArray result = this->databaseQuery
            .searchAnnotation(datasetId, id, contents, authorUsername, remoteAddress);

    if(result.isEmpty())
    {
        throw QString("NOT_FOUND");
    }
    return result;
}
