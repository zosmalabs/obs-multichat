#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "tiktok-client.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <curl/curl.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {
constexpr const char *USER_AGENT =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/144.0.0.0 Safari/537.36";

size_t writeBody(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *body = static_cast<std::string *>(userdata);
	if (body->size() + size * nmemb > 8 * 1024 * 1024)
		return 0;
	body->append(ptr, size * nmemb);
	return size * nmemb;
}

struct HttpInfo {
	long status = 0;
	QString contentType;
	CURLcode curlCode = CURLE_OK;
};

bool httpGet(const std::string &url, std::string &body, std::string *cookies = nullptr, HttpInfo *info = nullptr)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return false;
	body.clear();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.tiktok.com/");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	if (cookies && !cookies->empty())
		curl_easy_setopt(curl, CURLOPT_COOKIE, cookies->c_str());
	const CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (info) {
		info->status = status;
		info->curlCode = result;
		char *contentType = nullptr;
		curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
		info->contentType = QString::fromLatin1(contentType ? contentType : "");
	}
	curl_easy_cleanup(curl);
	return result == CURLE_OK && status >= 200 && status < 300;
}

QString describeResponse(const std::string &body, const HttpInfo &info)
{
	const QByteArray bytes(body.data(), static_cast<qsizetype>(body.size()));
	const QJsonDocument json = QJsonDocument::fromJson(bytes);
	if (json.isObject()) {
		const QJsonObject object = json.object();
		const QString code = object.value("statusCode").toVariant().toString();
		const QString message = object.value("message").toString().left(100);
		return QString::fromUtf8("JSON HTTP %1 (código %2, %3)")
			.arg(info.status)
			.arg(code.isEmpty() ? "?" : code, message.isEmpty() ? "sem mensagem" : message);
	}
	if (bytes.trimmed().startsWith('<'))
		return QString::fromUtf8("HTML HTTP %1 (%2)").arg(info.status).arg(info.contentType);
	return QString::fromUtf8("HTTP %1 (%2, %3 bytes)")
		.arg(info.status)
		.arg(info.contentType.isEmpty() ? "tipo ausente" : info.contentType)
		.arg(bytes.size());
}

std::string encoded(const QString &value)
{
	return QUrl::toPercentEncoding(value).toStdString();
}

bool readVarint(const std::string &data, size_t &pos, uint64_t &value)
{
	value = 0;
	int shift = 0;
	while (pos < data.size() && shift < 64) {
		const uint8_t byte = static_cast<uint8_t>(data[pos++]);
		value |= static_cast<uint64_t>(byte & 0x7f) << shift;
		if (!(byte & 0x80))
			return true;
		shift += 7;
	}
	return false;
}

bool readBytes(const std::string &data, size_t &pos, std::string &value)
{
	uint64_t length = 0;
	if (!readVarint(data, pos, length) || length > data.size() - pos)
		return false;
	value.assign(data.data() + pos, static_cast<size_t>(length));
	pos += static_cast<size_t>(length);
	return true;
}

bool skipField(const std::string &data, size_t &pos, int wire)
{
	uint64_t value = 0;
	std::string bytes;
	switch (wire) {
	case 0:
		return readVarint(data, pos, value);
	case 1:
		if (data.size() - pos < 8)
			return false;
		pos += 8;
		return true;
	case 2:
		return readBytes(data, pos, bytes);
	case 5:
		if (data.size() - pos < 4)
			return false;
		pos += 4;
		return true;
	default:
		return false;
	}
}

struct PollResponse {
	std::vector<std::pair<std::string, std::string>> messages;
	std::string cursor;
	std::string internalExt;
};

std::pair<QString, QString> parseChat(const std::string &data)
{
	QString name;
	QString comment;
	size_t pos = 0;
	while (pos < data.size()) {
		uint64_t key = 0;
		if (!readVarint(data, pos, key))
			break;
		const int field = static_cast<int>(key >> 3);
		const int wire = static_cast<int>(key & 7);
		if (field == 2 && wire == 2) {
			std::string user;
			if (!readBytes(data, pos, user))
				break;
			size_t upos = 0;
			QString nickname;
			QString uniqueId;
			while (upos < user.size()) {
				uint64_t ukey = 0;
				if (!readVarint(user, upos, ukey))
					break;
				const int ufield = static_cast<int>(ukey >> 3);
				const int uwire = static_cast<int>(ukey & 7);
				if ((ufield == 3 || ufield == 38) && uwire == 2) {
					std::string text;
					if (!readBytes(user, upos, text))
						break;
					if (ufield == 3)
						nickname = QString::fromUtf8(text);
					else
						uniqueId = QString::fromUtf8(text);
				} else if (!skipField(user, upos, uwire)) {
					break;
				}
			}
			name = nickname.isEmpty() ? uniqueId : nickname;
		} else if (field == 3 && wire == 2) {
			std::string text;
			if (!readBytes(data, pos, text))
				break;
			comment = QString::fromUtf8(text);
		} else if (!skipField(data, pos, wire)) {
			break;
		}
	}
	return {name, comment};
}

PollResponse parseResponse(const std::string &data)
{
	PollResponse response;
	size_t pos = 0;
	while (pos < data.size()) {
		uint64_t key = 0;
		if (!readVarint(data, pos, key))
			break;
		const int field = static_cast<int>(key >> 3);
		const int wire = static_cast<int>(key & 7);
		if (field == 1 && wire == 2) {
			std::string message;
			if (!readBytes(data, pos, message))
				break;
			std::string type;
			std::string binary;
			size_t mpos = 0;
			while (mpos < message.size()) {
				uint64_t mkey = 0;
				if (!readVarint(message, mpos, mkey))
					break;
				const int mfield = static_cast<int>(mkey >> 3);
				const int mwire = static_cast<int>(mkey & 7);
				if ((mfield == 1 || mfield == 2) && mwire == 2) {
					std::string value;
					if (!readBytes(message, mpos, value))
						break;
					if (mfield == 1)
						type = value;
					else
						binary = value;
				} else if (!skipField(message, mpos, mwire)) {
					break;
				}
			}
			if (type == "WebcastChatMessage" && !binary.empty())
				response.messages.push_back({type, binary});
		} else if ((field == 2 || field == 5) && wire == 2) {
			std::string value;
			if (!readBytes(data, pos, value))
				break;
			if (field == 2)
				response.cursor = value;
			else
				response.internalExt = value;
		} else if (!skipField(data, pos, wire)) {
			break;
		}
	}
	return response;
}

QString roomIdFromHtml(const std::string &html)
{
	const QString page = QString::fromUtf8(html);
	for (const QString &pattern :
	     {QStringLiteral(R"("roomId"\s*:\s*"?(\d+))"), QStringLiteral(R"("room_id"\s*:\s*"?(\d+))"),
	      QStringLiteral(R"(room_id=(\d+))")}) {
		const auto match = QRegularExpression(pattern).match(page);
		if (match.hasMatch())
			return match.captured(1);
	}
	return {};
}

std::string baseFetchUrl(const QString &roomId, const std::string &cursor, const std::string &internalExt)
{
	std::string url = "https://webcast.tiktok.com/webcast/im/fetch/?aid=1988&app_language=en-US&app_name=tiktok_web"
			  "&browser_language=en&browser_name=Mozilla&browser_online=true&browser_platform=Win32"
			  "&cookie_enabled=true&device_platform=web&focus_state=true&from_page=user&history_len=0"
			  "&is_fullscreen=false&is_page_visible=true&did_rule=3&fetch_rule=1&last_rtt=0&live_id=12"
			  "&resp_content_type=protobuf&screen_height=1152&screen_width=2048"
			  "&tz_name=America%2FSao_Paulo&webcast_sdk_version=1.3.0&update_version_code=1.3.0&room_id=" +
			  encoded(roomId);
	if (!cursor.empty())
		url += "&cursor=" + encoded(QString::fromStdString(cursor));
	if (!internalExt.empty())
		url += "&internal_ext=" + encoded(QString::fromStdString(internalExt));
	return url;
}

bool signInitialUrl(const std::string &url, std::string &signedUrl, std::string &cookies)
{
	const QString endpoint =
		QStringLiteral("https://tiktok.eulerstream.com/webcast/sign_url?client=ttlive-node&url=%1")
			.arg(QString::fromLatin1(QUrl::toPercentEncoding(QString::fromStdString(url))));
	std::string body;
	if (!httpGet(endpoint.toStdString(), body))
		return false;
	QJsonParseError error;
	const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject())
		return false;
	const QJsonObject object = doc.object();
	signedUrl = object.value("signedUrl").toString().toStdString();
	const QString token = object.value("msToken").toString();
	if (!token.isEmpty())
		cookies = "msToken=" + token.toStdString();
	return !signedUrl.empty();
}
} // namespace

QString TikTokClient::logPath()
{
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/obs-multichat-tiktok.log";
}

static void logTikTok(const QString &event)
{
	const QString path = TikTokClient::logPath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
		return;
	file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " UTC " + event + "\n").toUtf8());
}

static QString httpSummary(const HttpInfo &info, size_t bytes)
{
	return QString("HTTP %1, curl %2, tipo %3, %4 bytes")
		.arg(info.status)
		.arg(static_cast<int>(info.curlCode))
		.arg(info.contentType.isEmpty() ? QStringLiteral("ausente") : info.contentType)
		.arg(bytes);
}

TikTokClient::TikTokClient(QObject *parent) : QObject(parent)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

TikTokClient::~TikTokClient()
{
	stop();
}

void TikTokClient::stop()
{
	cancelled = true;
	if (worker.joinable())
		worker.join();
}

void TikTokClient::start(const QString &channel)
{
	stop();
	if (channel.isEmpty())
		return;
	cancelled = false;
	{
		QFile file(logPath());
		QDir().mkpath(QFileInfo(logPath()).absolutePath());
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
			file.write("OBS Multichat TikTok - diagnostico. Sem cookies, URLs assinadas ou mensagens.\n");
	}
	logTikTok(QString("Inicio: canal @%1").arg(channel));
	worker = std::thread([this, channel] {
		auto report = [this](const QString &text) {
			QMetaObject::invokeMethod(this, [this, text] { emit status(text); }, Qt::QueuedConnection);
		};
		auto emitChat = [this](const QString &name, const QString &text) {
			QMetaObject::invokeMethod(
				this, [this, name, text] { emit message(name, text); }, Qt::QueuedConnection);
		};

		while (!cancelled) {
			report(QString::fromUtf8("TikTok: localizando LIVE de @%1...").arg(channel));
			std::string html;
			const std::string liveUrl = "https://www.tiktok.com/@" + channel.toStdString() + "/live";
			HttpInfo liveInfo;
			const bool liveOk = httpGet(liveUrl, html, nullptr, &liveInfo);
			logTikTok("Pagina LIVE: " + httpSummary(liveInfo, html.size()));
			if (!liveOk) {
				report(QString::fromUtf8(
					"TikTok: não foi possível abrir a LIVE. Tentando novamente..."));
			} else {
				QString roomId = roomIdFromHtml(html);
				if (roomId.isEmpty()) {
					report(QString::fromUtf8(
						"TikTok: página sem roomId. Tentando API de fallback..."));
					std::string fallback;
					const std::string apiUrl =
						"https://www.tiktok.com/api-live/user/room/?aid=1988&app_name=tiktok_web"
						"&device_platform=web&sourceType=54&uniqueId=" +
						encoded(channel);
					HttpInfo fallbackInfo;
					const bool fallbackOk = httpGet(apiUrl, fallback, nullptr, &fallbackInfo);
					logTikTok("API roomId: " + httpSummary(fallbackInfo, fallback.size()));
					if (fallbackOk) {
						QJsonParseError jsonError;
						const QJsonDocument doc = QJsonDocument::fromJson(
							QByteArray::fromStdString(fallback), &jsonError);
						if (jsonError.error == QJsonParseError::NoError && doc.isObject()) {
							const QJsonObject data = doc.object().value("data").toObject();
							const QJsonObject user = data.value("user").toObject();
							roomId = user.value("roomId").toVariant().toString();
						}
					}
				}
				if (roomId.isEmpty()) {
					logTikTok("roomId ausente");
					report(QString::fromUtf8("TikTok: não foi possível obter o roomId da LIVE."));
				} else {
					logTikTok("roomId encontrado; iniciando polling");
					report(QString::fromUtf8("TikTok: LIVE encontrada. Iniciando polling HTTP..."));
					std::string cursor;
					std::string internalExt;
					std::string cookies;
					bool initial = true;
					bool needsSigning = false;
					int emptyResponses = 0;
					QSet<QByteArray> seen;
					QList<QByteArray> recent;
					while (!cancelled) {
						const std::string rawUrl = baseFetchUrl(roomId, cursor, internalExt);
						std::string url = rawUrl;
						std::string body;
						HttpInfo info;
						if (needsSigning && !signInitialUrl(rawUrl, url, cookies)) {
							logTikTok("Assinatura falhou");
							report(QString::fromUtf8(
								"TikTok: falha ao preparar polling HTTP. Reconectando..."));
							break;
						}
						bool fetchOk = httpGet(url, body, &cookies, &info);
						logTikTok(QString("Polling %1: ")
								  .arg(needsSigning ? "assinado" : "direto") +
							  httpSummary(info, body.size()));
						if (!needsSigning && (!fetchOk || body.empty())) {
							logTikTok(
								body.empty() && fetchOk
									? "Polling direto vazio; tentando assinatura"
									: "Polling direto recusado; tentando assinatura");
							if (signInitialUrl(rawUrl, url, cookies)) {
								needsSigning = true;
								fetchOk = httpGet(url, body, &cookies, &info);
								logTikTok("Polling assinado: " +
									  httpSummary(info, body.size()));
							} else {
								logTikTok(
									"Assinatura falhou apos resposta direta vazia/recusada");
							}
						}
						if (!fetchOk) {
							logTikTok("Polling recusado: " +
								  httpSummary(info, body.size()));
							report(QString::fromUtf8(
								       "TikTok: polling recusado: %1. Reconectando...")
								       .arg(describeResponse(body, info)));
							break;
						}
						const PollResponse response = parseResponse(body);
						logTikTok(
							QString("Resposta: cursor=%1, extensao=%2, eventos=%3, tentativa_vazia=%4")
								.arg(!response.cursor.empty())
								.arg(!response.internalExt.empty())
								.arg(response.messages.size())
								.arg(emptyResponses + 1));
						if (response.cursor.empty() && response.messages.empty()) {
							++emptyResponses;
							report(QString::fromUtf8(
								       "TikTok: resposta sem cursor/chat (%1; tentativa %2/5)")
								       .arg(describeResponse(body, info))
								       .arg(emptyResponses));
							if (emptyResponses >= 5)
								break;
							for (int i = 0; i < 20 && !cancelled; ++i)
								std::this_thread::sleep_for(
									std::chrono::milliseconds(100));
							continue;
						}
						emptyResponses = 0;
						if (!response.cursor.empty())
							cursor = response.cursor;
						if (!response.internalExt.empty())
							internalExt = response.internalExt;
						for (const auto &item : response.messages) {
							const QByteArray digest = QCryptographicHash::hash(
								QByteArray(item.second.data(),
									   static_cast<qsizetype>(item.second.size())),
								QCryptographicHash::Sha256);
							if (seen.contains(digest))
								continue;
							seen.insert(digest);
							recent.append(digest);
							if (recent.size() > 1000)
								seen.remove(recent.takeFirst());
							if (initial)
								continue;
							const auto chat = parseChat(item.second);
							if (!chat.first.isEmpty() && !chat.second.isEmpty())
								emitChat(chat.first, chat.second);
						}
						initial = false;
						report(QString::fromUtf8(
							       "TikTok: polling HTTP ativo em @%1. Aguardando comentários.")
							       .arg(channel));
						for (int i = 0; i < 10 && !cancelled; ++i)
							std::this_thread::sleep_for(std::chrono::milliseconds(100));
					}
				}
			}
			for (int i = 0; i < 30 && !cancelled; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});
}
