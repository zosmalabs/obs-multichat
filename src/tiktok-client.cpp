#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "tiktok-client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
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
	body->append(ptr, size * nmemb);
	return size * nmemb;
}

bool httpGet(const std::string &url, std::string &body, std::string *cookies = nullptr)
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
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	if (cookies && !cookies->empty())
		curl_easy_setopt(curl, CURLOPT_COOKIE, cookies->c_str());
	const CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return result == CURLE_OK && status >= 200 && status < 300;
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
	QRegularExpressionMatch match = QRegularExpression(QStringLiteral("\\\"roomId\\\":\\\"?(\\\\d+)\\\"?")).match(page);
	if (match.hasMatch())
		return match.captured(1);
	match = QRegularExpression(QStringLiteral("\\\"room_id\\\":\\\"?(\\\\d+)\\\"?")).match(page);
	return match.hasMatch() ? match.captured(1) : QString();
}

std::string baseFetchUrl(const QString &roomId, const std::string &cursor, const std::string &internalExt)
{
	std::string url =
		"https://webcast.tiktok.com/webcast/im/fetch/?aid=1988&app_language=en-US&app_name=tiktok_web"
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
	worker = std::thread([this, channel] {
		auto report = [this](const QString &text) {
			QMetaObject::invokeMethod(this, [this, text] { emit status(text); }, Qt::QueuedConnection);
		};
		auto emitChat = [this](const QString &name, const QString &text) {
			QMetaObject::invokeMethod(this, [this, name, text] { emit message(name, text); }, Qt::QueuedConnection);
		};

		while (!cancelled) {
			report(QString::fromUtf8("TikTok: localizando LIVE de @%1...").arg(channel));
			std::string html;
			const std::string liveUrl = "https://www.tiktok.com/@" + channel.toStdString() + "/live";
			if (!httpGet(liveUrl, html)) {
				report(QString::fromUtf8("TikTok: não foi possível abrir a LIVE. Tentando novamente..."));
			} else {
				const QString roomId = roomIdFromHtml(html);
				if (roomId.isEmpty()) {
					report(QString::fromUtf8("TikTok: LIVE não encontrada ou TikTok bloqueou a consulta."));
				} else {
					report(QString::fromUtf8("TikTok: LIVE encontrada. Iniciando polling HTTP..."));
					std::string cursor;
					std::string internalExt;
					std::string cookies;
					std::string firstUrl;
					if (!signInitialUrl(baseFetchUrl(roomId, cursor, internalExt), firstUrl, cookies)) {
						report(QString::fromUtf8("TikTok: falha ao iniciar polling. Tentando novamente..."));
					} else {
						bool initial = true;
						while (!cancelled) {
							std::string body;
							const std::string url =
								initial ? firstUrl : baseFetchUrl(roomId, cursor, internalExt);
							if (!httpGet(url, body, &cookies)) {
								report(QString::fromUtf8("TikTok: polling interrompido. Reconectando..."));
								break;
							}
							const PollResponse response = parseResponse(body);
							if (initial && response.cursor.empty()) {
								report(QString::fromUtf8("TikTok: resposta de polling inválida. Reconectando..."));
								break;
							}
							initial = false;
							if (!response.cursor.empty())
								cursor = response.cursor;
							if (!response.internalExt.empty())
								internalExt = response.internalExt;
							report(QString::fromUtf8("TikTok: polling ativo em @%1. Aguardando comentários.")
								       .arg(channel));
							for (const auto &item : response.messages) {
								const auto chat = parseChat(item.second);
								if (!chat.first.isEmpty() && !chat.second.isEmpty())
									emitChat(chat.first, chat.second);
							}
							for (int i = 0; i < 10 && !cancelled; ++i)
								std::this_thread::sleep_for(std::chrono::milliseconds(100));
						}
					}
				}
			}
			for (int i = 0; i < 30 && !cancelled; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});
}
