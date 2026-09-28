#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QTextDocument>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSpinBox>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>
#include <QRegularExpression>
#include <QUrl>
#include <QSslSocket>
#include <QTimer>
#include <QRandomGenerator>
#include "kick-client.h"
#include "hidden-browser.h"
#include "capture-script.h"
#include "youtube-client.h"
#include <QUrlQuery>
#include <QStringList>
#include <curl/curl.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static bool dock_registered = false;
static QCheckBox *overlay_checkbox = nullptr;
static QLabel *overlay_status = nullptr;
static QJsonArray overlay_messages;
static int overlay_max_lines = 8;
static int overlay_font_size = 32;
static bool overlay_nick_colors = true;
static bool overlay_cards = true;
static QColor overlay_background = QColor(0, 0, 0, 0);
static bool shutting_down = false;
static QTcpServer *overlay_server = nullptr;
static QList<QPointer<QTcpSocket>> overlay_clients;
static constexpr const char *overlay_name = "Zosma Multichat Web";
static QPointer<QTextBrowser> panel_view;
static QPointer<QLabel> image_status;
static QPointer<QLabel> capture_status;
static QList<QJsonObject> panel_history;
static QHash<QString, QImage> image_cache;
static QHash<QString, QByteArray> image_bytes;
static QHash<QString, QString> image_tokens;
static QHash<QString, QString> token_urls;
static QSet<QString> pending_images;
static QSet<QString> failed_images;
static QJsonObject twitch_badge_images;
static QSet<QString> loaded_badge_catalogs;
static QSet<QString> channel_badge_keys;
static QHash<QString, QJsonArray> captured_badges;
static QPointer<QWidget> capture_window;
static QPointer<QCefWidget> twitch_capture;
static QPointer<QCefWidget> kick_capture;
static QCef *capture_cef = nullptr;
static QString capture_twitch_channel;
static QString capture_kick_channel;
static QHash<QString, QString> capture_state;
struct AssetRequest {
	CURL *handle = nullptr;
	QByteArray bytes;
	QString url;
	QString room_id;
	bool catalog = false;
};
static CURLM *asset_multi = nullptr;
static QTimer *asset_timer = nullptr;
static QHash<CURL *, AssetRequest *> asset_requests;

static void renderPanel();
static void requestChatImage(const QString &url);
static void receiveCapturedBadges(const QByteArray &bytes);
static void startCapture(const QString &twitch, const QString &kick);

static void showCaptureState()
{
	if (capture_status)
		capture_status->setText(QString::fromUtf8("Captura das badges · Twitch: %1 · Kick: %2")
						.arg(capture_state.value("Twitch", "aguardando"),
						     capture_state.value("Kick", "aguardando")));
}

static void receiveCaptureTitle(const QString &title)
{
	if (!title.startsWith("zosma:") || title.size() > 100000)
		return;
	const QByteArray bytes = QByteArray::fromBase64(title.mid(6).toLatin1());
	const QJsonObject payload = QJsonDocument::fromJson(bytes).object();
	const QString platform = payload.value("platform").toString();
	if (platform != "Twitch" && platform != "Kick")
		return;
	if (payload.value("type").toString() == "status") {
		const QString url = payload.value("url").toString();
		if (QUrl(url).host() != "kick.com" && QUrl(url).host() != "www.kick.com" &&
		    QUrl(url).host() != "www.twitch.tv" && QUrl(url).host() != "twitch.tv") {
			capture_state.insert(platform,
					     QString::fromUtf8("endereço inesperado: %1").arg(QUrl(url).host()));
		} else {
			capture_state.insert(platform, QString::fromUtf8("chat ativo (%1 linhas, %2 badges visíveis)")
							       .arg(payload.value("rows").toInt())
							       .arg(payload.value("found").toInt()));
		}
		showCaptureState();
	} else {
		receiveCapturedBadges(bytes);
	}
}

static void stopCapture()
{
	if (capture_window) {
		if (twitch_capture)
			twitch_capture->closeBrowser();
		if (kick_capture)
			kick_capture->closeBrowser();
		delete capture_window;
	}
	twitch_capture = nullptr;
	kick_capture = nullptr;
	capture_window = nullptr;
	delete capture_cef;
	capture_cef = nullptr;
}

static QString cssColor(const QColor &color)
{
	return QString("rgba(%1,%2,%3,%4)")
		.arg(color.red())
		.arg(color.green())
		.arg(color.blue())
		.arg(color.alphaF(), 0, 'f', 3);
}

static QByteArray overlayPayload()
{
	QJsonObject payload;
	payload.insert("limit", overlay_max_lines);
	payload.insert("messages", overlay_messages);
	payload.insert("badgeImages", twitch_badge_images);
	QJsonObject assets;
	for (auto it = image_tokens.cbegin(); it != image_tokens.cend(); ++it)
		if (image_bytes.contains(it.key()))
			assets.insert(it.key(), "/asset/" + it.value());
	payload.insert("assets", assets);
	payload.insert("fontSize", overlay_font_size);
	payload.insert("nickColors", overlay_nick_colors);
	payload.insert("cards", overlay_cards);
	payload.insert("background", cssColor(overlay_background));
	return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

static constexpr const char *overlay_html = R"HTML(<!doctype html><html><meta charset="utf-8"><style>
html,body{margin:0;overflow:hidden;font:32px Arial,sans-serif;color:#fff;background:transparent}
#chat{display:flex;flex-direction:column;justify-content:flex-end;height:100vh;gap:9px;padding:16px;box-sizing:border-box}
.line{overflow-wrap:anywhere;text-shadow:0 2px 4px #0009,0 0 2px #0008;line-height:1.3}
.cards .line{background:#111c;border-left:4px solid var(--color);border-radius:8px;padding:7px 12px;box-shadow:0 2px 10px #0003}
.badge{display:inline-grid;place-items:center;width:1.45em;height:1.45em;border-radius:6px;vertical-align:middle;margin-right:8px;background:var(--color);color:#fff;font:bold .68em Arial,sans-serif;text-shadow:0 1px 2px #0008}
.role{display:inline-block;background:#ffffff26;border-radius:4px;font:.55em Arial,sans-serif;padding:2px 5px;vertical-align:middle;margin-right:4px}
.role-image{width:1em;height:1em;object-fit:contain;vertical-align:middle;margin-right:4px}
.emote{height:1.15em;vertical-align:middle;object-fit:contain}
.nick{color:var(--nick-color);font-weight:700}.message{white-space:pre-wrap}
</style><div id="chat"></div><script>
const colors={Twitch:'#9146ff',Kick:'#53fc18',YouTube:'#ff0033',TikTok:'#ffffff'};
const badges={Twitch:'T',Kick:'K',YouTube:'▶',TikTok:'♪'};
const chat=document.getElementById('chat');
function draw(data){document.body.style.background=data.background;document.body.style.fontSize=data.fontSize+'px';
 chat.classList.toggle('cards',data.cards);chat.replaceChildren();for(const item of data.messages.slice(-data.limit)){
 const row=document.createElement('div');row.className='line';row.style.setProperty('--color',colors[item.platform]||'#fff');
 row.style.setProperty('--nick-color',data.nickColors?(colors[item.platform]||'#fff'):'#fff');
 const badge=document.createElement('span');badge.className='badge';badge.textContent=badges[item.platform]||'?';badge.title=item.platform;
 const nick=document.createElement('span');nick.className='nick';nick.textContent=item.name+': ';
 row.append(badge);
 for(const role of item.badges||[]){const url=role.image||data.badgeImages[role.key];
  if(url){const img=document.createElement('img');img.className='role-image';img.src=data.assets[url]||url;img.title=role.label;
   img.onerror=()=>img.replaceWith(document.createTextNode('['+role.label+']'));row.append(img)}
  else {const tag=document.createElement('span');tag.className='role';tag.textContent=role.label;row.append(tag)}
 }
 row.append(nick);
 const message=document.createElement('span');message.className='message';let offset=0;
 for(const emote of item.emotes||[]){if(emote.start<offset||emote.end>item.message.length)continue;
  message.append(document.createTextNode(item.message.slice(offset,emote.start)));
  const img=document.createElement('img');img.className='emote';img.src=data.assets[emote.url]||emote.url;img.alt=emote.label;img.title=emote.label;
  img.onerror=()=>img.replaceWith(document.createTextNode(emote.label));message.append(img);offset=emote.end;
 }
 message.append(document.createTextNode(item.message.slice(offset)));row.append(message);chat.append(row);
}}
const events=new EventSource('/events');events.onmessage=e=>draw(JSON.parse(e.data));
</script></html>)HTML";

static bool startOverlayServer(QWidget *parent)
{
	if (overlay_server)
		return true;
	overlay_server = new QTcpServer(parent);
	if (!overlay_server->listen(QHostAddress::LocalHost)) {
		delete overlay_server;
		overlay_server = nullptr;
		return false;
	}
	QObject::connect(overlay_server, &QTcpServer::newConnection, parent, [parent]() {
		while (overlay_server && overlay_server->hasPendingConnections()) {
			QTcpSocket *socket = overlay_server->nextPendingConnection();
			QObject::connect(socket, &QTcpSocket::readyRead, parent, [socket]() {
				if (socket->property("stream").toBool())
					return;
				if (!socket->canReadLine())
					return;
				const QByteArray request = socket->readLine();
				socket->readAll();
				if (!request.startsWith("GET ") || !request.contains("HTTP/1.")) {
					socket->disconnectFromHost();
					return;
				}
				if (request.startsWith("GET /events ")) {
					socket->write(
						"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n\r\n");
					socket->write("data: " + overlayPayload() + "\n\n");
					overlay_clients.append(socket);
					socket->setProperty("stream", true);
					QObject::connect(socket, &QTcpSocket::disconnected, socket,
							 &QObject::deleteLater);
				} else if (request.startsWith("GET /asset/")) {
					const QString token = QString::fromLatin1(request.mid(11, 64));
					const QByteArray body = image_bytes.value(token_urls.value(token));
					if (body.isEmpty()) {
						socket->write(
							"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
					} else {
						const char *mime = body.startsWith("GIF8")       ? "image/gif"
								   : body.startsWith("\x89PNG")  ? "image/png"
								   : body.startsWith("\xff\xd8") ? "image/jpeg"
								   : body.startsWith("RIFF")
									   ? "image/webp"
									   : "application/octet-stream";
						socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + QByteArray(mime) +
							      "\r\nContent-Length: " + QByteArray::number(body.size()) +
							      "\r\nConnection: close\r\n\r\n" + body);
					}
					socket->disconnectFromHost();
				} else {
					const QByteArray body(overlay_html);
					socket->write(
						"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
						QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
						body);
					socket->disconnectFromHost();
				}
			});
		}
	});
	return true;
}

static void refreshOverlay()
{
	if (shutting_down)
		return;
	const QByteArray event = "data: " + overlayPayload() + "\n\n";
	for (auto it = overlay_clients.begin(); it != overlay_clients.end();) {
		if (!*it || (*it)->state() != QAbstractSocket::ConnectedState)
			it = overlay_clients.erase(it);
		else {
			(*it)->write(event);
			++it;
		}
	}
}

static QImage platformIcon(const QString &platform)
{
	QImage icon(32, 32, QImage::Format_ARGB32_Premultiplied);
	icon.fill(Qt::transparent);
	QPainter painter(&icon);
	painter.setRenderHint(QPainter::Antialiasing);
	const QColor color = platform == "Twitch"    ? QColor("#9146ff")
			     : platform == "Kick"    ? QColor("#53b824")
			     : platform == "YouTube" ? QColor("#ff0033")
						     : QColor("#555555");
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	painter.drawRoundedRect(QRectF(1, 1, 30, 30), 6, 6);
	painter.setPen(Qt::white);
	QFont font = painter.font();
	font.setBold(true);
	font.setPixelSize(23);
	painter.setFont(font);
	painter.drawText(icon.rect(), Qt::AlignCenter,
			 platform == "YouTube" ? QString::fromUtf8("▶") : platform.left(1));
	return icon;
}

static size_t collectAsset(char *data, size_t size, size_t count, void *context)
{
	auto *request = static_cast<AssetRequest *>(context);
	const size_t length = size * count;
	if (request->bytes.size() + length > 2 * 1024 * 1024)
		return 0;
	request->bytes.append(data, static_cast<qsizetype>(length));
	return length;
}

static void queueAsset(const QString &url, bool catalog = false, const QString &room_id = {})
{
	if (!asset_multi)
		return;
	auto *request = new AssetRequest;
	request->url = url;
	request->catalog = catalog;
	request->room_id = room_id;
	request->handle = curl_easy_init();
	if (!request->handle) {
		delete request;
		return;
	}
	const QByteArray encoded = url.toUtf8();
	curl_easy_setopt(request->handle, CURLOPT_URL, encoded.constData());
	curl_easy_setopt(request->handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(request->handle, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(request->handle, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(request->handle, CURLOPT_USERAGENT, "Mozilla/5.0 OBS-Multichat/0.8");
	curl_easy_setopt(request->handle, CURLOPT_WRITEFUNCTION, collectAsset);
	curl_easy_setopt(request->handle, CURLOPT_WRITEDATA, request);
	asset_requests.insert(request->handle, request);
	curl_multi_add_handle(asset_multi, request->handle);
	asset_timer->start();
}

static void processBadgeCatalog(const QByteArray &bytes, const QString &room_id)
{
	const QJsonArray sets = QJsonDocument::fromJson(bytes).array();
	for (const QJsonValue set_value : sets) {
		const QJsonObject set = set_value.toObject();
		for (const QJsonValue version_value : set.value("versions").toArray()) {
			const QJsonObject version = version_value.toObject();
			const QString image = version.value("image_url_2x").toString();
			const QString key = set.value("id").toString() + "/" + version.value("id").toString();
			if (QUrl(image).host() == "static-cdn.jtvnw.net" &&
			    (!room_id.isEmpty() || !channel_badge_keys.contains(key))) {
				twitch_badge_images.insert(key, image);
				if (!room_id.isEmpty())
					channel_badge_keys.insert(key);
			}
		}
	}
	renderPanel();
	refreshOverlay();
}

static void pollAssets()
{
	if (!asset_multi)
		return;
	int running = 0;
	curl_multi_perform(asset_multi, &running);
	int remaining = 0;
	while (CURLMsg *result = curl_multi_info_read(asset_multi, &remaining)) {
		if (result->msg != CURLMSG_DONE)
			continue;
		AssetRequest *request = asset_requests.take(result->easy_handle);
		if (!request)
			continue;
		long status = 0;
		curl_easy_getinfo(request->handle, CURLINFO_RESPONSE_CODE, &status);
		const CURLcode curl_result = result->data.result;
		curl_multi_remove_handle(asset_multi, request->handle);
		curl_easy_cleanup(request->handle);
		if (curl_result == CURLE_OK && status == 200) {
			if (request->catalog) {
				processBadgeCatalog(request->bytes, request->room_id);
			} else {
				QImage image;
				if (image.loadFromData(request->bytes)) {
					image_cache.insert(request->url, image);
					image_bytes.insert(request->url, request->bytes);
					renderPanel();
					refreshOverlay();
				} else
					failed_images.insert(request->url);
			}
		} else if (!request->catalog)
			failed_images.insert(request->url);
		else if (image_status)
			image_status->setText(QString::fromUtf8("Catálogo de badges da Twitch indisponível."));
		if (!request->catalog)
			pending_images.remove(request->url);
		if (image_status && !request->catalog)
			image_status->setText(QString::fromUtf8("Imagens carregadas: %1 · indisponíveis: %2")
						      .arg(image_cache.size())
						      .arg(failed_images.size()));
		delete request;
	}
	if (asset_requests.isEmpty())
		asset_timer->stop();
}

static void requestChatImage(const QString &url)
{
	if (!asset_multi || image_cache.contains(url) || pending_images.contains(url) || failed_images.contains(url) ||
	    image_cache.size() + pending_images.size() >= 200)
		return;
	const QUrl parsed(url);
	if (parsed.scheme() != "https" ||
	    (parsed.host() != "static-cdn.jtvnw.net" && parsed.host() != "files.kick.com" &&
	     parsed.host() != "cdn.kick.com" && parsed.host() != "ext.cdn.kick.com"))
		return;
	pending_images.insert(url);
	const QString token =
		QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex());
	image_tokens.insert(url, token);
	token_urls.insert(token, url);
	queueAsset(url);
}

static QString panelImage(const QString &url, const QString &label, int height)
{
	if (!image_cache.contains(url)) {
		requestChatImage(url);
		return label.toHtmlEscaped();
	}
	const QUrl resource(QString("asset:%1").arg(qHash(url)));
	panel_view->document()->addResource(QTextDocument::ImageResource, resource, image_cache.value(url));
	return QString("<img src='%1' height='%2' alt='%3'>")
		.arg(resource.toString(), QString::number(height), label.toHtmlEscaped());
}

static QString panelMessageHtml(const QJsonObject &entry)
{
	const QString message = entry.value("message").toString();
	QString result;
	int offset = 0;
	for (const QJsonValue value : entry.value("emotes").toArray()) {
		const QJsonObject emote = value.toObject();
		const int start = emote.value("start").toInt(-1);
		const int end = emote.value("end").toInt(-1);
		if (start < offset || end <= start || end > message.size())
			continue;
		result += message.mid(offset, start - offset).toHtmlEscaped();
		result += panelImage(emote.value("url").toString(), emote.value("label").toString(), 23);
		offset = end;
	}
	return result + message.mid(offset).toHtmlEscaped();
}

static void renderPanel()
{
	if (!panel_view)
		return;
	auto *scroll = panel_view->verticalScrollBar();
	const bool at_bottom = scroll->value() >= scroll->maximum() - 24;
	const int previous = scroll->value();
	QString html = "<html><body style='color:white;font-family:Arial;font-size:13px'>";
	for (const QJsonObject &entry : panel_history) {
		const QString platform = entry.value("platform").toString();
		const QString color = platform == "Twitch" ? "#9146ff" : platform == "Kick" ? "#53fc18" : "#ff0033";
		const QUrl resource(QString("platform:%1").arg(platform.toLower()));
		panel_view->document()->addResource(QTextDocument::ImageResource, resource, platformIcon(platform));
		html += QString("<p style='margin:4px 0'><img src='%1' width='18' height='18'> ")
				.arg(resource.toString());
		for (const QJsonValue value : entry.value("badges").toArray()) {
			const QJsonObject badge = value.toObject();
			const QString label = badge.value("label").toString();
			QString url = badge.value("image").toString();
			if (url.isEmpty())
				url = twitch_badge_images.value(badge.value("key").toString()).toString();
			if (!url.isEmpty())
				html += panelImage(url, label, 17) + " ";
			else
				html += QString("<span style='color:#cccccc'>[%1]</span> ").arg(label.toHtmlEscaped());
		}
		html += QString("<b style='color:%1'>%2:</b> %3</p>")
				.arg(color, entry.value("name").toString().toHtmlEscaped(), panelMessageHtml(entry));
	}
	panel_view->setHtml(html + "</body></html>");
	scroll->setValue(at_bottom ? scroll->maximum() : previous);
}

static bool attachOverlay()
{
	if (shutting_down || !overlay_server || !overlay_server->isListening())
		return false;
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return false;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene) {
		obs_source_release(scene_source);
		return false;
	}
	obs_source_t *old_source = obs_get_source_by_name("Zosma Multichat");
	if (old_source) {
		obs_source_remove(old_source);
		obs_source_release(old_source);
	}
	obs_source_t *overlay_source = obs_get_source_by_name(overlay_name);
	const QString url = QString("http://127.0.0.1:%1/").arg(overlay_server->serverPort());
	if (!overlay_source) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", url.toUtf8().constData());
		obs_data_set_int(settings, "width", 900);
		obs_data_set_int(settings, "height", 1080);
		overlay_source = obs_source_create("browser_source", overlay_name, settings, nullptr);
		obs_data_release(settings);
	} else {
		obs_data_t *settings = obs_source_get_settings(overlay_source);
		if (url != QString::fromUtf8(obs_data_get_string(settings, "url"))) {
			obs_data_set_string(settings, "url", url.toUtf8().constData());
			obs_source_update(overlay_source, settings);
		}
		obs_data_release(settings);
	}
	if (overlay_source && !obs_scene_find_source(scene, overlay_name))
		obs_scene_add(scene, overlay_source);
	if (overlay_source)
		obs_source_set_enabled(overlay_source, true);
	const bool attached = overlay_source != nullptr;
	if (overlay_source)
		obs_source_release(overlay_source);
	obs_source_release(scene_source);
	return attached;
}

static void receiveCapturedBadges(const QByteArray &bytes)
{
	const QJsonObject payload = QJsonDocument::fromJson(bytes).object();
	const QString platform = payload.value("platform").toString();
	const QString name = payload.value("name").toString().left(80);
	if ((platform != "Kick" && platform != "Twitch") || name.isEmpty())
		return;
	QJsonArray badges;
	for (const QJsonValue value : payload.value("badges").toArray()) {
		if (badges.size() >= 15)
			break;
		const QJsonObject badge = value.toObject();
		QString url = badge.value("image").toString();
		if (url.startsWith("data:image/png;base64,")) {
			const QByteArray image_data = QByteArray::fromBase64(url.mid(22).toLatin1());
			QImage image;
			if (image_data.size() > 32 * 1024 || !image.loadFromData(image_data))
				continue;
			url = "capture:" +
			      QString::fromLatin1(
				      QCryptographicHash::hash(image_data, QCryptographicHash::Sha256).toHex());
			image_cache.insert(url, image);
			image_bytes.insert(url, image_data);
			const QString token = url.mid(8);
			image_tokens.insert(url, token);
			token_urls.insert(token, url);
		} else if (QUrl(url).scheme() != "https" ||
			   (QUrl(url).host() != "static-cdn.jtvnw.net" && QUrl(url).host() != "files.kick.com" &&
			    QUrl(url).host() != "cdn.kick.com" && QUrl(url).host() != "ext.cdn.kick.com")) {
			continue;
		}
		badges.append(QJsonObject{{"label", badge.value("label").toString().left(40)}, {"image", url}});
	}
	if (badges.isEmpty())
		return;
	const QString key = platform + ":" + name.toLower();
	if (captured_badges.size() > 200)
		captured_badges.clear();
	captured_badges.insert(key, badges);
	for (QJsonObject &entry : panel_history)
		if (entry.value("platform").toString() == platform &&
		    entry.value("name").toString().compare(name, Qt::CaseInsensitive) == 0)
			entry.insert("badges", badges);
	for (int i = 0; i < overlay_messages.size(); ++i) {
		QJsonObject entry = overlay_messages[i].toObject();
		if (entry.value("platform").toString() == platform &&
		    entry.value("name").toString().compare(name, Qt::CaseInsensitive) == 0) {
			entry.insert("badges", badges);
			overlay_messages.replace(i, entry);
		}
	}
	if (image_status)
		image_status->setText(QString::fromUtf8("Imagens carregadas: %1 · indisponíveis: %2")
					      .arg(image_cache.size())
					      .arg(failed_images.size()));
	renderPanel();
	refreshOverlay();
}

static void appendChat(const QString &platform, const QString &name, const QString &message,
		       const QJsonArray &badges = {}, const QJsonArray &emotes = {})
{
	QJsonObject entry;
	entry.insert("platform", platform);
	entry.insert("name", name.left(80));
	entry.insert("message", message.left(500));
	entry.insert("badges", captured_badges.value(platform + ":" + name.toLower(), badges));
	entry.insert("emotes", emotes);
	overlay_messages.append(entry);
	while (overlay_messages.size() > 50)
		overlay_messages.removeAt(0);
	panel_history.append(entry);
	while (panel_history.size() > 100)
		panel_history.removeFirst();
	renderPanel();
	if (overlay_checkbox && overlay_checkbox->isChecked())
		refreshOverlay();
}

static QJsonArray kickEmotes(const QString &message)
{
	QJsonArray result;
	QRegularExpression pattern("\\[emote:([0-9]+):([^\\]]+)\\]");
	auto matches = pattern.globalMatch(message);
	while (matches.hasNext()) {
		const auto match = matches.next();
		QJsonObject emote;
		emote.insert("start", match.capturedStart());
		emote.insert("end", match.capturedEnd());
		emote.insert("label", match.captured(2));
		emote.insert("url", "https://files.kick.com/emotes/" + match.captured(1) + "/fullsize");
		result.append(emote);
	}
	return result;
}

static QJsonArray twitchEmotes(const QString &message, const QByteArray &tag)
{
	QMap<int, QJsonObject> positions;
	for (const QByteArray &group : tag.split('/')) {
		const int separator = group.indexOf(':');
		if (separator < 1)
			continue;
		const QByteArray id = group.left(separator);
		if (!QRegularExpression("^[0-9]+$").match(QString::fromLatin1(id)).hasMatch())
			continue;
		for (const QByteArray &range : group.mid(separator + 1).split(',')) {
			const QList<QByteArray> ends = range.split('-');
			if (ends.size() != 2)
				continue;
			bool ok_start = false, ok_end = false;
			const int start = ends[0].toInt(&ok_start);
			const int end = ends[1].toInt(&ok_end) + 1;
			if (!ok_start || !ok_end || start < 0 || end > message.size() || end <= start)
				continue;
			QJsonObject emote;
			emote.insert("start", start);
			emote.insert("end", end);
			emote.insert("label", message.mid(start, end - start));
			emote.insert("url", "https://static-cdn.jtvnw.net/emoticons/v2/" + QString::fromLatin1(id) +
						    "/default/dark/2.0");
			positions.insert(start, emote);
		}
	}
	QJsonArray result;
	for (const QJsonObject &emote : positions)
		result.append(emote);
	return result;
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		shutting_down = true;
		stopCapture();
		if (overlay_server)
			overlay_server->close();
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		startCapture(capture_twitch_channel, capture_kick_channel);
		if (overlay_checkbox && overlay_checkbox->isChecked() && attachOverlay() && overlay_status)
			overlay_status->setText(QString::fromUtf8("Fonte Zosma Multichat Web ativa na cena atual."));
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		if (overlay_checkbox && overlay_checkbox->isChecked() && attachOverlay() && overlay_status)
			overlay_status->setText(QString::fromUtf8("Fonte Zosma Multichat Web ativa na cena atual."));
	}
}

static QString twitchChannel(const QString &input)
{
	QString channel = input.trimmed();
	if (channel.startsWith('@'))
		channel.remove(0, 1);
	if (channel.startsWith("https://")) {
		QUrl url(channel);
		const QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
		channel = parts.value(parts.value(0) == "popout" ? 1 : 0);
	}
	return QRegularExpression("^[a-zA-Z0-9_]+$").match(channel).hasMatch() ? channel.toLower() : QString();
}

static QString youtubeVideoId(const QString &input)
{
	QUrl url(input.trimmed());
	if (url.scheme() != "https")
		return {};
	QString host = url.host().toLower();
	if (host.startsWith("www."))
		host.remove(0, 4);
	QString id;
	if (host == "youtu.be")
		id = url.path().section('/', 1, 1);
	else if (host == "youtube.com") {
		if (url.path() == "/watch" || url.path() == "/live_chat")
			id = QUrlQuery(url).queryItemValue("v");
		else if (url.path().startsWith("/live/"))
			id = url.path().section('/', 2, 2);
	}
	return QRegularExpression("^[a-zA-Z0-9_-]{11}$").match(id).hasMatch() ? id : QString();
}

static QString ircUnescape(QString value)
{
	return value.replace("\\s", " ")
		.replace("\\:", ";")
		.replace("\\r", "\r")
		.replace("\\n", "\n")
		.replace("\\\\", "\\");
}

static QString kickChannel(const QString &input)
{
	QString channel = input.trimmed();
	if (channel.startsWith('@'))
		channel.remove(0, 1);
	if (channel.startsWith("https://")) {
		QUrl url(channel);
		if (url.host() != "kick.com" && url.host() != "www.kick.com")
			return {};
		QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
		int popout = parts.indexOf("popout");
		channel = popout >= 0
				  ? parts.value(popout + 1)
				  : parts.value(parts.size() >= 2 && parts.last() == "chatroom" ? parts.size() - 2 : 0);
	}
	return QRegularExpression("^[a-zA-Z0-9_-]+$").match(channel).hasMatch() ? channel.toLower() : QString();
}

static bool validSource(int platform, const QString &value)
{
	const QString input = value.trimmed();
	if (input.isEmpty())
		return true;
	if (platform == 0 || platform == 1 || platform == 3) {
		if (platform == 0 && !twitchChannel(input).isEmpty())
			return true;
		if (platform != 0 && QRegularExpression("^@?[a-zA-Z0-9_.-]+$").match(input).hasMatch())
			return true;
	}
	QUrl url(input);
	if (!url.isValid() || url.scheme() != "https")
		return false;
	QString host = url.host().toLower();
	if (host.startsWith("www."))
		host.remove(0, 4);
	switch (platform) {
	case 0:
		return host == "twitch.tv" && !twitchChannel(input).isEmpty();
	case 1:
		return host == "kick.com" && !kickChannel(input).isEmpty();
	case 2:
		return !youtubeVideoId(input).isEmpty();
	case 3:
		return host == "tiktok.com" && url.path().startsWith("/@");
	}
	return false;
}

static void startCapture(const QString &twitch, const QString &kick)
{
	if (shutting_down || !overlay_server || (!capture_window && twitch.isEmpty() && kick.isEmpty()))
		return;
	if (!capture_cef) {
		obs_module_t *module = obs_get_module("obs-browser");
		if (!module)
			return;
		using CreateCef = QCef *(*)();
		const auto create =
			reinterpret_cast<CreateCef>(os_dlsym(obs_get_module_lib(module), "obs_browser_create_qcef"));
		if (!create)
			return;
		capture_cef = create();
		if (!capture_cef)
			return;
		capture_cef->init_browser();
	}
	if (!capture_cef->initialized())
		return;
	if (!capture_window) {
		capture_window = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint);
		capture_window->setAttribute(Qt::WA_ShowWithoutActivating);
		capture_window->setGeometry(-3000, -3000, 760, 600);
		capture_window->show();
	}
	const QByteArray script(capture_script);
	auto update = [&](QPointer<QCefWidget> &widget, const QString &channel, const QString &url, int x) {
		if (channel.isEmpty()) {
			if (widget) {
				widget->closeBrowser();
				delete widget;
				widget = nullptr;
			}
			return;
		}
		const bool created = !widget;
		if (created) {
			widget = capture_cef->create_widget(capture_window, url.toStdString());
			if (!widget)
				return;
			QObject::connect(widget, &QCefWidget::titleChanged, capture_window,
					 [](const QString &title) { receiveCaptureTitle(title); });
			QObject::connect(widget, &QCefWidget::urlChanged, capture_window,
					 [platform = x == 0 ? "Twitch" : "Kick"](const QString &loaded) {
						 if (loaded != "about:blank") {
							 capture_state.insert(
								 platform, loaded.startsWith("data:")
										   ? "falha ao carregar página"
										   : "página aberta, aguardando chat");
							 showCaptureState();
						 }
					 });
			widget->setGeometry(x, 0, 380, 600);
			widget->setStartupScript(script.toStdString());
			widget->allowAllPopups(false);
			widget->show();
		}
		if (!created)
			widget->setURL(url.toStdString());
	};
	if (twitch != capture_twitch_channel || !twitch_capture) {
		capture_twitch_channel = twitch;
		update(twitch_capture, twitch, "https://www.twitch.tv/popout/" + twitch + "/chat?popout=", 0);
	}
	if (kick != capture_kick_channel || !kick_capture) {
		capture_kick_channel = kick;
		update(kick_capture, kick, "https://kick.com/popout/" + kick + "/chat", 380);
	}
}

bool obs_module_load(void)
{
	// OBS creates the dock and its toggle in Exibir > Paineis.
	auto *body = new QWidget();
	auto *layout = new QVBoxLayout(body);
	auto *form = new QFormLayout();
	constexpr const char *keys[] = {"twitch", "kick", "youtube", "tiktok"};
	constexpr const char *labels[] = {"Twitch", "Kick", "YouTube", "TikTok"};
	QLineEdit *inputs[4];
	QSettings settings("Zosma", "OBS Multichat");
	overlay_font_size = qBound(16, settings.value("overlay_font_size", 32).toInt(), 64);
	overlay_nick_colors = settings.value("overlay_nick_colors", true).toBool();
	overlay_cards = settings.value("overlay_cards", true).toBool();
	overlay_background = QColor(settings.value("overlay_background", "#00000000").toString());
	if (!overlay_background.isValid())
		overlay_background = QColor(0, 0, 0, 0);
	for (int i = 0; i < 4; ++i) {
		inputs[i] = new QLineEdit(body);
		inputs[i]->setPlaceholderText(i == 2 ? "Link da live ou do chat" : "@canal ou link");
		inputs[i]->setText(settings.value(keys[i]).toString());
		form->addRow(labels[i], inputs[i]);
	}
	layout->addLayout(form);
	auto *overlay = new QCheckBox(QString::fromUtf8("Exibir na transmissão"), body);
	overlay_checkbox = overlay;
	overlay->setChecked(settings.value("overlay", false).toBool());
	layout->addWidget(overlay);
	overlay_max_lines = qBound(1, settings.value("overlay_max_lines", 8).toInt(), 30);
	auto *max_lines = new QSpinBox(body);
	max_lines->setRange(1, 30);
	max_lines->setValue(overlay_max_lines);
	max_lines->setSuffix(QString::fromUtf8(" linhas"));
	form->addRow(QString::fromUtf8("Linhas na transmissão"), max_lines);
	QObject::connect(max_lines, qOverload<int>(&QSpinBox::valueChanged), body, [](int count) {
		overlay_max_lines = count;
		QSettings settings("Zosma", "OBS Multichat");
		settings.setValue("overlay_max_lines", count);
		refreshOverlay();
	});
	overlay_status = new QLabel(QString::fromUtf8("Fonte de navegador nas cenas usadas durante a live."), body);
	overlay_status->setWordWrap(true);
	layout->addWidget(overlay_status);
	QObject::connect(overlay, &QCheckBox::toggled, body, [](bool enabled) {
		QSettings settings("Zosma", "OBS Multichat");
		settings.setValue("overlay", enabled);
		if (enabled) {
			if (overlay_status)
				overlay_status->setText(
					attachOverlay()
						? QString::fromUtf8("Fonte Zosma Multichat Web ativa na cena atual.")
						: QString::fromUtf8(
							  "Não foi possível adicionar a fonte à cena atual."));
		} else {
			obs_source_t *overlay_source = obs_get_source_by_name(overlay_name);
			if (overlay_source) {
				obs_source_set_enabled(overlay_source, false);
				obs_source_release(overlay_source);
			}
			if (overlay_status)
				overlay_status->setText(QString::fromUtf8("Exibição na transmissão desativada."));
		}
	});
	auto *status = new QLabel(QString::fromUtf8("Configure as fontes para o primeiro teste."), body);
	status->setWordWrap(true);
	layout->addWidget(status);
	auto *save = new QPushButton(QString::fromUtf8("Salvar fontes"), body);
	layout->addWidget(save);
	auto *messages = new QTextBrowser(body);
	panel_view = messages;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	asset_multi = curl_multi_init();
	asset_timer = new QTimer(body);
	asset_timer->setInterval(80);
	QObject::connect(asset_timer, &QTimer::timeout, body, pollAssets);
	messages->setOpenExternalLinks(false);
	messages->setPlaceholderText(
		QString::fromUtf8("As mensagens aparecerão aqui quando a captura for implementada."));
	QColor panel_background(settings.value("panel_background", "#161922").toString());
	if (!panel_background.isValid())
		panel_background = QColor("#161922");
	messages->setStyleSheet(
		QString("QTextBrowser {background-color: %1; color: white; border-radius: 8px; padding: 8px;}")
			.arg(panel_background.name()));
	layout->addWidget(messages);
	image_status = new QLabel(QString::fromUtf8("Imagens carregadas: 0"), body);
	layout->addWidget(image_status);
	capture_status = new QLabel(body);
	capture_status->setWordWrap(true);
	layout->addWidget(capture_status);
	showCaptureState();
	auto *appearance = new QGroupBox(QString::fromUtf8("Aparência"), body);
	auto *appearance_form = new QFormLayout(appearance);
	auto *font_size = new QSpinBox(appearance);
	font_size->setRange(16, 64);
	font_size->setSuffix(" px");
	font_size->setValue(overlay_font_size);
	appearance_form->addRow(QString::fromUtf8("Letra na transmissão"), font_size);
	QObject::connect(font_size, qOverload<int>(&QSpinBox::valueChanged), body, [](int size) {
		overlay_font_size = size;
		QSettings("Zosma", "OBS Multichat").setValue("overlay_font_size", size);
		refreshOverlay();
	});
	auto *layout_style = new QComboBox(appearance);
	layout_style->addItem(QString::fromUtf8("Cartões"));
	layout_style->addItem(QString::fromUtf8("Simples"));
	layout_style->setCurrentIndex(overlay_cards ? 0 : 1);
	appearance_form->addRow(QString::fromUtf8("Estilo"), layout_style);
	QObject::connect(layout_style, qOverload<int>(&QComboBox::currentIndexChanged), body, [](int index) {
		overlay_cards = index == 0;
		QSettings("Zosma", "OBS Multichat").setValue("overlay_cards", overlay_cards);
		refreshOverlay();
	});
	auto *nick_colors = new QCheckBox(QString::fromUtf8("Nicks nas cores das plataformas"), appearance);
	nick_colors->setChecked(overlay_nick_colors);
	appearance_form->addRow(nick_colors);
	QObject::connect(nick_colors, &QCheckBox::toggled, body, [](bool enabled) {
		overlay_nick_colors = enabled;
		QSettings("Zosma", "OBS Multichat").setValue("overlay_nick_colors", enabled);
		refreshOverlay();
	});
	auto *overlay_color =
		new QPushButton(QString::fromUtf8("Selecionar cor (transparente por padrão)"), appearance);
	appearance_form->addRow(QString::fromUtf8("Fundo da transmissão"), overlay_color);
	QObject::connect(overlay_color, &QPushButton::clicked, body, [body]() {
		const QColor selected = QColorDialog::getColor(overlay_background, body,
							       QString::fromUtf8("Fundo da transmissão"),
							       QColorDialog::ShowAlphaChannel);
		if (!selected.isValid())
			return;
		overlay_background = selected;
		QSettings("Zosma", "OBS Multichat").setValue("overlay_background", selected.name(QColor::HexArgb));
		refreshOverlay();
	});
	auto *panel_color = new QPushButton(QString::fromUtf8("Selecionar cor"), appearance);
	appearance_form->addRow(QString::fromUtf8("Fundo do painel"), panel_color);
	QObject::connect(panel_color, &QPushButton::clicked, body, [body, messages, panel_background]() mutable {
		const QColor selected =
			QColorDialog::getColor(panel_background, body, QString::fromUtf8("Fundo do painel"));
		if (!selected.isValid())
			return;
		panel_background = selected;
		QSettings("Zosma", "OBS Multichat").setValue("panel_background", selected.name());
		messages->setStyleSheet(
			QString("QTextBrowser {background-color: %1; color: white; border-radius: 8px; padding: 8px;}")
				.arg(selected.name()));
	});
	layout->insertWidget(layout->indexOf(messages), appearance);
	auto *kick = new KickClient(body);
	auto *youtube = new YouTubeClient(body);
	QObject::connect(youtube, &YouTubeClient::status, body,
			 [status](const QString &message) { status->setText(message); });
	QObject::connect(youtube, &YouTubeClient::message, body,
			 [](const QString &name, const QString &message) { appendChat("YouTube", name, message); });
	QObject::connect(kick, &KickClient::status, body,
			 [status](const QString &message) { status->setText(message); });
	QObject::connect(kick, &KickClient::message, body,
			 [](const QString &name, const QString &message, const QJsonArray &raw_badges) {
				 QJsonArray badges;
				 for (const QJsonValue value : raw_badges) {
					 const QJsonObject badge = value.toObject();
					 const QString label =
						 badge.value("text").toString(badge.value("type").toString());
					 if (label.isEmpty())
						 continue;
					 QJsonObject display{{"label", label.left(40)}};
					 const QString url = badge.value("image_url").toString();
					 if (QUrl(url).scheme() == "https" && (QUrl(url).host() == "files.kick.com" ||
									       QUrl(url).host() == "cdn.kick.com"))
						 display.insert("image", url);
					 badges.append(display);
				 }
				 appendChat("Kick", name, message, badges, kickEmotes(message));
			 });
	auto *socket = new QSslSocket(body);
	auto *retry = new QTimer(body);
	retry->setSingleShot(true);
	QString *channel = new QString();
	QByteArray *buffer = new QByteArray();
	bool *plainMode = new bool(!QSslSocket::supportsSsl());
	QObject::connect(body, &QObject::destroyed, [channel, buffer, plainMode]() {
		delete channel;
		delete buffer;
		delete plainMode;
	});
	auto connectChat = [socket, channel, plainMode, status]() {
		if (channel->isEmpty() || socket->state() != QAbstractSocket::UnconnectedState)
			return;
		status->setText(QString::fromUtf8("Twitch: conectando a #%1...").arg(*channel));
		if (*plainMode)
			socket->connectToHost("irc.chat.twitch.tv", 6667);
		else
			socket->connectToHostEncrypted("irc.chat.twitch.tv", 6697);
	};
	QObject::connect(retry, &QTimer::timeout, socket, connectChat);
	auto joinChat = [socket, channel, status, plainMode]() {
		socket->write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
		socket->write("NICK justinfan" +
			      QByteArray::number(QRandomGenerator::global()->bounded(100000, 999999)) + "\r\n");
		socket->write("JOIN #" + channel->toUtf8() + "\r\n");
		status->setText(
			*plainMode ? QString::fromUtf8("Twitch: conectado a #%1 sem TLS. Aguardando mensagens.")
					     .arg(*channel)
				   : QString::fromUtf8("Twitch: conectado a #%1. Aguardando mensagens.").arg(*channel));
	};
	QObject::connect(socket, &QSslSocket::encrypted, body, joinChat);
	QObject::connect(socket, &QSslSocket::connected, body, [plainMode, joinChat]() {
		if (*plainMode)
			joinChat();
	});
	QObject::connect(socket, &QSslSocket::readyRead, body, [socket, buffer]() {
		buffer->append(socket->readAll());
		if (buffer->size() > 65536)
			buffer->clear();
		while (true) {
			int end = buffer->indexOf("\r\n");
			if (end < 0)
				break;
			QByteArray line = buffer->left(end);
			buffer->remove(0, end + 2);
			if (line.startsWith("PING ")) {
				socket->write("PONG " + line.mid(5) + "\r\n");
				continue;
			}
			int command = line.indexOf(" PRIVMSG #");
			if (command < 0)
				continue;
			int content = line.indexOf(" :", command + 1);
			if (content < 0)
				continue;
			QString name;
			QHash<QByteArray, QByteArray> tags_map;
			if (line.startsWith('@')) {
				QByteArray tags = line.mid(1, line.indexOf(' ') - 1);
				for (const auto &tag : tags.split(';')) {
					const int equals = tag.indexOf('=');
					if (equals > 0)
						tags_map.insert(tag.left(equals), tag.mid(equals + 1));
				}
				name = ircUnescape(QString::fromUtf8(tags_map.value("display-name")));
			}
			if (name.isEmpty()) {
				int prefix = line.indexOf(" :");
				int bang = line.indexOf('!', prefix + 2);
				if (prefix >= 0 && bang > prefix)
					name = QString::fromUtf8(line.mid(prefix + 2, bang - prefix - 2));
			}
			if (name.isEmpty())
				continue;
			const QString message = QString::fromUtf8(line.mid(content + 2));
			QJsonArray badges;
			for (const QByteArray &badge : tags_map.value("badges").split(',')) {
				if (badge.isEmpty())
					continue;
				const QString key = QString::fromLatin1(badge);
				badges.append(QJsonObject{{"key", key}, {"label", key.section('/', 0, 0)}});
			}
			appendChat("Twitch", name, message, badges, twitchEmotes(message, tags_map.value("emotes")));
		}
	});
	QObject::connect(socket, &QSslSocket::disconnected, body, [retry, channel, status]() {
		if (!channel->isEmpty()) {
			status->setText(QString::fromUtf8("Twitch: desconectado. Reconectando..."));
			retry->start(5000);
		}
	});
	QObject::connect(socket, &QSslSocket::errorOccurred, body,
			 [socket, status, plainMode, retry](QAbstractSocket::SocketError) {
				 if (!*plainMode && (!QSslSocket::supportsSsl() ||
						     socket->errorString().contains("TLS initialization failed",
										    Qt::CaseInsensitive))) {
					 *plainMode = true;
					 status->setText(QString::fromUtf8(
						 "Twitch: TLS indisponível. Tentando conexão de leitura sem TLS..."));
					 socket->abort();
					 retry->start(100);
					 return;
				 }
				 status->setText(QString::fromUtf8("Twitch: %1").arg(socket->errorString()));
			 });
	auto *test = new QPushButton(QString::fromUtf8("Testar painel"), body);
	layout->addWidget(test);
	QObject::connect(
		save, &QPushButton::clicked, body,
		[inputs, status, socket, retry, channel, buffer, connectChat, kick, youtube]() {
			constexpr const char *names[] = {"Twitch", "Kick", "YouTube", "TikTok"};
			constexpr const char *keys[] = {"twitch", "kick", "youtube", "tiktok"};
			QSettings settings("Zosma", "OBS Multichat");
			for (int i = 0; i < 4; ++i) {
				if (!validSource(i, inputs[i]->text())) {
					status->setText(QString::fromUtf8("Endereço inválido em %1.").arg(names[i]));
					return;
				}
			}
			for (int i = 0; i < 4; ++i)
				settings.setValue(keys[i], inputs[i]->text().trimmed());
			settings.sync();
			if (settings.status() != QSettings::NoError) {
				status->setText(QString::fromUtf8("Falha ao salvar as fontes."));
				return;
			}
			*channel = twitchChannel(inputs[0]->text());
			const QString kick_name = kickChannel(inputs[1]->text());
			kick->start(kick_name);
			startCapture(*channel, kick_name);
			youtube->start(youtubeVideoId(inputs[2]->text()));
			buffer->clear();
			retry->stop();
			socket->abort();
			if (!channel->isEmpty()) {
				connectChat();
			} else if (inputs[1]->text().trimmed().isEmpty() && inputs[2]->text().trimmed().isEmpty()) {
				status->setText(QString::fromUtf8(
					"Fontes salvas. Preencha Twitch para iniciar a captura real."));
			}
		});
	*channel = twitchChannel(inputs[0]->text());
	capture_twitch_channel = *channel;
	capture_kick_channel = kickChannel(inputs[1]->text());
	kick->start(capture_kick_channel);
	youtube->start(youtubeVideoId(inputs[2]->text()));
	if (!channel->isEmpty())
		connectChat();
	QObject::connect(test, &QPushButton::clicked, body, []() {
		appendChat("Twitch", "exemplo", "Olá, chat! Kappa",
			   QJsonArray{QJsonObject{{"key", "premium/1"}, {"label", "Prime"}}},
			   QJsonArray{QJsonObject{{"start", 11},
						  {"end", 16},
						  {"label", "Kappa"},
						  {"url",
						   "https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/2.0"}}});
		appendChat("Kick", "exemplo", "Bem-vindos! [emote:4148074:HYPERCLAP]",
			   QJsonArray{QJsonObject{{"label", "Subscriber"}}},
			   kickEmotes("Bem-vindos! [emote:4148074:HYPERCLAP]"));
		appendChat("YouTube", "exemplo", "Mensagem de teste. 😃");
	});
	if (!obs_frontend_add_dock_by_id("zosma-multichat", "Multichat", body)) {
		delete body;
		overlay_checkbox = nullptr;
		overlay_status = nullptr;
		return false;
	}
	startOverlayServer(body);
	auto *capture_retry = new QTimer(body);
	capture_retry->setInterval(6000);
	QObject::connect(capture_retry, &QTimer::timeout, body, []() {
		if ((!capture_twitch_channel.isEmpty() && !twitch_capture) ||
		    (!capture_kick_channel.isEmpty() && !kick_capture))
			startCapture(capture_twitch_channel, capture_kick_channel);
	});
	capture_retry->start();
	startCapture(capture_twitch_channel, capture_kick_channel);
	dock_registered = true;
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_log(LOG_INFO, "Multichat test dock loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	overlay_checkbox = nullptr;
	overlay_status = nullptr;
	overlay_messages = QJsonArray();
	panel_history.clear();
	panel_view = nullptr;
	image_status = nullptr;
	capture_status = nullptr;
	capture_state.clear();
	if (asset_timer)
		asset_timer->stop();
	stopCapture();
	captured_badges.clear();
	for (AssetRequest *request : asset_requests) {
		curl_multi_remove_handle(asset_multi, request->handle);
		curl_easy_cleanup(request->handle);
		delete request;
	}
	asset_requests.clear();
	if (asset_multi)
		curl_multi_cleanup(asset_multi);
	asset_multi = nullptr;
	asset_timer = nullptr;
	image_cache.clear();
	image_bytes.clear();
	image_tokens.clear();
	token_urls.clear();
	pending_images.clear();
	failed_images.clear();
	twitch_badge_images = QJsonObject();
	loaded_badge_catalogs.clear();
	channel_badge_keys.clear();
	if (overlay_server)
		overlay_server->close();
	overlay_server = nullptr;
	overlay_clients.clear();
	if (dock_registered) {
		obs_frontend_remove_dock("zosma-multichat");
		dock_registered = false;
	}
}
