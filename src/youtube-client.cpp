#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "youtube-client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <curl/curl.h>
#include <algorithm>
#include <chrono>

namespace {
size_t appendResponse(char *ptr, size_t size, size_t count, void *output)
{
	auto *result = static_cast<std::string *>(output);
	result->append(ptr, size * count);
	return size * count;
}
struct Response {
	std::string body;
	long status = 0;
	CURLcode error = CURLE_OK;
};
Response request(const std::string &url, const std::string *payload = nullptr)
{
	Response response;
	CURL *curl = curl_easy_init();
	if (!curl) {
		response.error = CURLE_FAILED_INIT;
		return response;
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,
			 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/126.0.0.0 Safari/537.36");
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
	headers = curl_slist_append(headers, "Cookie: SOCS=CAI");
	if (payload) {
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload->size()));
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	response.error = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return response;
}
QString extract(const QString &page, const QRegularExpression &expression)
{
	return expression.match(page).captured(1);
}
QString messageText(const QJsonObject &renderer)
{
	QString text;
	for (const QJsonValue run : renderer.value("message").toObject().value("runs").toArray()) {
		QJsonObject item = run.toObject();
		QString part = item.value("text").toString();
		if (part.isEmpty()) {
			QJsonObject emoji = item.value("emoji").toObject();
			QJsonArray shortcuts = emoji.value("shortcuts").toArray();
			part = shortcuts.isEmpty() ? QString() : shortcuts.at(0).toString();
			if (part.isEmpty())
				part = emoji.value("emojiId").toString();
		}
		text += part;
	}
	return text;
}
} // namespace

YouTubeClient::YouTubeClient(QObject *parent) : QObject(parent)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}
YouTubeClient::~YouTubeClient()
{
	stop();
}
void YouTubeClient::stop()
{
	cancelled = true;
	if (worker.joinable())
		worker.join();
}
void YouTubeClient::start(const QString &videoId)
{
	stop();
	if (videoId.isEmpty())
		return;
	cancelled = false;
	worker = std::thread([this, videoId] {
		auto report = [this](QString text) {
			QMetaObject::invokeMethod(this, [this, text] { emit status(text); }, Qt::QueuedConnection);
		};
		report(QString::fromUtf8("YouTube: localizando o chat da live..."));
		Response page = request("https://www.youtube.com/watch?v=" + videoId.toStdString());
		if (cancelled)
			return;
		if (page.error != CURLE_OK || page.status != 200) {
			report(QString::fromUtf8("YouTube: não foi possível abrir a live (%1).").arg(page.status));
			return;
		}
		QString html = QString::fromUtf8(page.body.data(), page.body.size());
		if (html.contains(QRegularExpression("[\\\"']isReplay[\\\"']\\s*:\\s*true"))) {
			report(QString::fromUtf8("YouTube: esta live já terminou."));
			return;
		}
		QString key = extract(
			html, QRegularExpression("[\\\"']INNERTUBE_API_KEY[\\\"']\\s*:\\s*[\\\"']([^\\\"']+)[\\\"']"));
		QString version = extract(
			html, QRegularExpression("[\\\"']clientVersion[\\\"']\\s*:\\s*[\\\"']([0-9.]+)[\\\"']"));
		QString continuation = extract(
			html,
			QRegularExpression(
				"\\\"liveChatRenderer\\\"\\s*:\\s*\\{.*?\\\"continuations\\\"\\s*:\\s*\\[\\s*\\{\\s*\\\"reloadContinuationData\\\"\\s*:\\s*\\{\\s*\\\"continuation\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"",
				QRegularExpression::DotMatchesEverythingOption));
		if (key.isEmpty() || version.isEmpty() || continuation.isEmpty()) {
			report(QString::fromUtf8("YouTube: chat indisponível ou formato da página alterado."));
			return;
		}
		report(QString::fromUtf8("YouTube: conectado à live. Aguardando mensagens."));
		QSet<QString> seen;
		int failures = 0;
		while (!cancelled) {
			QJsonObject client{{"clientName", "WEB"}, {"clientVersion", version}};
			QJsonObject context{{"client", client}};
			QJsonObject body{{"context", context}, {"continuation", continuation}};
			std::string payload = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
			Response chat = request("https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=" +
							key.toStdString(),
						&payload);
			if (cancelled)
				return;
			int waitMs = 10000;
			if (chat.error != CURLE_OK || chat.status != 200) {
				++failures;
				waitMs = (std::min)(60000, 10000 * failures);
				report(QString::fromUtf8("YouTube: falha na consulta (%1). Tentando novamente...")
					       .arg(chat.status));
			} else {
				QJsonObject data =
					QJsonDocument::fromJson(QByteArray::fromStdString(chat.body)).object();
				QJsonObject live = data.value("continuationContents")
							   .toObject()
							   .value("liveChatContinuation")
							   .toObject();
				if (live.isEmpty()) {
					report(QString::fromUtf8(
						"YouTube: a live terminou ou o chat ficou indisponível."));
					return;
				}
				failures = 0;
				for (const QJsonValue action : live.value("actions").toArray()) {
					QJsonObject item = action.toObject()
								   .value("addChatItemAction")
								   .toObject()
								   .value("item")
								   .toObject();
					QJsonObject renderer = item.value("liveChatTextMessageRenderer").toObject();
					if (renderer.isEmpty())
						renderer = item.value("liveChatPaidMessageRenderer").toObject();
					QString id = renderer.value("id").toString();
					QString name =
						renderer.value("authorName").toObject().value("simpleText").toString();
					QString text = messageText(renderer);
					if (id.isEmpty() || name.isEmpty() || text.isEmpty() || seen.contains(id))
						continue;
					seen.insert(id);
					QMetaObject::invokeMethod(
						this, [this, name, text] { emit message(name, text); },
						Qt::QueuedConnection);
				}
				if (seen.size() > 3000)
					seen.clear();
				QJsonArray continuations = live.value("continuations").toArray();
				QJsonObject next = continuations.isEmpty() ? QJsonObject()
									   : continuations.at(0).toObject();
				QJsonObject token = next.value("invalidationContinuationData").toObject();
				if (token.isEmpty())
					token = next.value("timedContinuationData").toObject();
				continuation = token.value("continuation").toString();
				if (continuation.isEmpty()) {
					report(QString::fromUtf8("YouTube: o chat foi encerrado."));
					return;
				}
				waitMs = std::clamp(token.value("timeoutMs").toInt(10000), 1000, 60000);
			}
			for (int elapsed = 0; elapsed < waitMs && !cancelled; elapsed += 100)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});
}
