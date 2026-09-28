#include "kick-client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <curl/curl.h>
#include <chrono>

namespace {
size_t appendResponse(char *ptr, size_t size, size_t count, void *output)
{
	auto *result = static_cast<std::string *>(output);
	result->append(ptr, size * count);
	return size * count;
}
void configure(CURL *curl, const char *url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 OBS-Multichat/0.3");
}
int chatroomId(const std::string &slug)
{
	for (const char *version : {"v1", "v2"}) {
		CURL *curl = curl_easy_init();
		if (!curl)
			return 0;
		std::string response;
		std::string url = "https://kick.com/api/" + std::string(version) + "/channels/" + slug;
		configure(curl, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		CURLcode result = curl_easy_perform(curl);
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_cleanup(curl);
		if (result != CURLE_OK || code != 200)
			continue;
		QJsonObject data = QJsonDocument::fromJson(QByteArray::fromStdString(response)).object();
		int id = data.value("chatroom").toObject().value("id").toInt();
		if (id > 0)
			return id;
	}
	return 0;
}
} // namespace

KickClient::KickClient(QObject *parent) : QObject(parent)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}
KickClient::~KickClient()
{
	stop();
}
void KickClient::stop()
{
	cancelled = true;
	if (worker.joinable())
		worker.join();
}
void KickClient::start(const QString &channel)
{
	stop();
	if (channel.isEmpty())
		return;
	cancelled = false;
	worker = std::thread([this, slug = channel.toStdString()] {
		auto report = [this](QString text) {
			QMetaObject::invokeMethod(this, [this, text] { emit status(text); }, Qt::QueuedConnection);
		};
		report(QString::fromUtf8("Kick: procurando a sala do canal..."));
		int id = chatroomId(slug);
		if (cancelled)
			return;
		if (!id) {
			report(QString::fromUtf8(
				"Kick: não foi possível obter a sala. A página pode exigir verificação no navegador."));
			return;
		}
		while (!cancelled) {
			CURL *curl = curl_easy_init();
			if (!curl)
				break;
			const char *url =
				"wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679?protocol=7&client=js&version=8.4.0-rc2&flash=false";
			configure(curl, url);
			curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
			CURLcode result = curl_easy_perform(curl);
			if (result == CURLE_OK) {
				report(QString::fromUtf8("Kick: conectado. Inscrevendo no chat..."));
				std::string receive;
				auto lastPing = std::chrono::steady_clock::now();
				while (!cancelled) {
					char bytes[16384];
					size_t length = 0;
					const curl_ws_frame *frame = nullptr;
					result = curl_ws_recv(curl, bytes, sizeof(bytes), &length, &frame);
					if (result != CURLE_OK && result != CURLE_AGAIN)
						break;
					if (result == CURLE_OK && length) {
						receive.append(bytes, length);
						if (receive.size() > 262144) {
							receive.clear();
							break;
						}
						if (frame && frame->bytesleft == 0 && !receive.empty()) {
							QJsonObject event = QJsonDocument::fromJson(
										    QByteArray::fromStdString(receive))
										    .object();
							receive.clear();
							QString type = event.value("event").toString();
							std::string reply;
							if (type == "pusher:connection_established") {
								reply = "{\"event\":\"pusher:subscribe\",\"data\":{\"auth\":\"\",\"channel\":\"chatrooms." +
									std::to_string(id) + ".v2\"}}";
							} else if (type == "pusher_internal:subscription_succeeded") {
								report(QString::fromUtf8(
									"Kick: sala conectada. Aguardando mensagens."));
							} else if (type == "pusher:ping") {
								reply = "{\"event\":\"pusher:pong\",\"data\":{}}";
							} else if (type == "App\\Events\\ChatMessageEvent" ||
								   type == "App\\Events\\ChatMessageSentEvent") {
								QJsonObject payload =
									QJsonDocument::fromJson(
										event.value("data").toString().toUtf8())
										.object();
								QString name = payload.value("sender")
										       .toObject()
										       .value("username")
										       .toString();
								QString text = payload.value("content").toString();
								if (!name.isEmpty() && !text.isEmpty())
									QMetaObject::invokeMethod(
										this,
										[this, name, text] {
											emit message(name, text);
										},
										Qt::QueuedConnection);
							}
							if (!reply.empty()) {
								size_t sent = 0;
								if (curl_ws_send(curl, reply.data(), reply.size(),
										 &sent, 0, CURLWS_TEXT) != CURLE_OK)
									break;
							}
						} else if (frame && frame->bytesleft == 0) {
							receive.clear();
						}
					}
					auto now = std::chrono::steady_clock::now();
					if (now - lastPing > std::chrono::seconds(30)) {
						const std::string ping = "{\"event\":\"pusher:ping\",\"data\":{}}";
						size_t sent = 0;
						if (curl_ws_send(curl, ping.data(), ping.size(), &sent, 0,
								 CURLWS_TEXT) != CURLE_OK)
							break;
						lastPing = now;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(80));
				}
			}
			curl_easy_cleanup(curl);
			if (cancelled)
				break;
			report(QString::fromUtf8("Kick: conexão interrompida. Reconectando..."));
			for (int i = 0; i < 30 && !cancelled; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});
}
