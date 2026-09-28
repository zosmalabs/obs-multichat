#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "tiktok-client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <curl/curl.h>
#include <chrono>

namespace {
void configure(CURL *curl, const std::string &url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,
			 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/144.0.0.0 Safari/537.36");
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
}

QString firstString(const QJsonObject &object, std::initializer_list<const char *> keys)
{
	for (const char *key : keys) {
		const QString value = object.value(key).toString();
		if (!value.isEmpty())
			return value;
	}
	return {};
}

void collectChat(const QJsonValue &value, QList<QPair<QString, QString>> &out)
{
	if (value.isArray()) {
		for (const QJsonValue item : value.toArray())
			collectChat(item, out);
		return;
	}
	if (!value.isObject())
		return;

	const QJsonObject object = value.toObject();
	const QString type = firstString(object, {"type", "event", "eventType", "method"});
	const QJsonObject data =
		object.value("data").toObject().isEmpty() ? object : object.value("data").toObject();

	if (type.contains("Chat", Qt::CaseInsensitive) || type == "chat") {
		QJsonObject user = data.value("user").toObject();
		if (user.isEmpty())
			user = data.value("userInfo").toObject();
		QString name = firstString(user, {"nickname", "uniqueId", "displayName", "username"});
		if (name.isEmpty())
			name = firstString(data, {"nickname", "uniqueId", "username"});
		QString text = firstString(data, {"comment", "content", "text", "message"});
		if (!name.isEmpty() && !text.isEmpty())
			out.append({name, text});
	}

	for (const char *key : {"events", "messages", "data"}) {
		const QJsonValue nested = object.value(key);
		if ((nested.isArray() || nested.isObject()) && nested != value)
			collectChat(nested, out);
	}
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
		int retrySeconds = 3;
		while (!cancelled) {
			report(QString::fromUtf8("TikTok: conectando a @%1...").arg(channel));
			const std::string url =
				"wss://ws.eulerstream.com/?uniqueId=" + channel.toStdString() +
				"&features.bundleEvents=false&features.rawMessages=false"
				"&features.normalizeUniqueId=true&features.schemaVersion=v2"
				"&features.webcastPlatform=web";
			CURL *curl = curl_easy_init();
			if (!curl) {
				report(QString::fromUtf8("TikTok: falha ao iniciar conexão."));
				return;
			}
			configure(curl, url);
			CURLcode result = curl_easy_perform(curl);
			long response = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
			if (result == CURLE_OK) {
				retrySeconds = 3;
				report(QString::fromUtf8("TikTok: conectado a @%1. Aguardando comentários.")
					       .arg(channel));
				std::string frame;
				while (!cancelled) {
					char bytes[65536];
					size_t length = 0;
					const curl_ws_frame *meta = nullptr;
					result = curl_ws_recv(curl, bytes, sizeof(bytes), &length, &meta);
					if (result == CURLE_AGAIN) {
						std::this_thread::sleep_for(std::chrono::milliseconds(60));
						continue;
					}
					if (result != CURLE_OK)
						break;
					if (length)
						frame.append(bytes, length);
					if (!meta || meta->bytesleft != 0)
						continue;
					if (meta->flags & CURLWS_CLOSE)
						break;
					if (meta->flags & CURLWS_PING) {
						size_t sent = 0;
						curl_ws_send(curl, frame.data(), frame.size(), &sent, 0, CURLWS_PONG);
						frame.clear();
						continue;
					}
					if (!(meta->flags & CURLWS_TEXT)) {
						frame.clear();
						continue;
					}
					const QByteArray payload(frame.data(), static_cast<qsizetype>(frame.size()));
					frame.clear();
					QJsonParseError parseError;
					const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
					if (parseError.error != QJsonParseError::NoError)
						continue;
					QList<QPair<QString, QString>> chats;
					if (document.isArray())
						collectChat(document.array(), chats);
					else
						collectChat(document.object(), chats);
					for (const auto &chat : chats) {
						const QString name = chat.first;
						const QString text = chat.second;
						QMetaObject::invokeMethod(
							this, [this, name, text] { emit message(name, text); },
							Qt::QueuedConnection);
					}
				}
			} else {
				report(QString::fromUtf8("TikTok: conexão recusada (%1, HTTP %2). Tentando novamente...")
					       .arg(curl_easy_strerror(result))
					       .arg(response));
			}
			curl_easy_cleanup(curl);
			if (cancelled)
				break;
			for (int elapsed = 0; elapsed < retrySeconds * 10 && !cancelled; ++elapsed)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			retrySeconds = (std::min)(30, retrySeconds * 2);
		}
	});
}
