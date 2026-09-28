#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
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
#include "youtube-client.h"
#include <QUrlQuery>
#include <QStringList>

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
 const message=document.createElement('span');message.className='message';message.textContent=item.message;
 row.append(badge,nick,message);chat.append(row);
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
				if (!socket->canReadLine())
					return;
				const QByteArray request = socket->readLine();
				socket->readAll();
				if (socket->property("stream").toBool())
					return;
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

static void updateOverlay(const QString &platform, const QString &name, const QString &message)
{
	QJsonObject entry;
	entry.insert("platform", platform);
	entry.insert("name", name.left(80));
	entry.insert("message", message.left(500));
	overlay_messages.append(entry);
	while (overlay_messages.size() > 50)
		overlay_messages.removeAt(0);
	if (overlay_checkbox && overlay_checkbox->isChecked())
		refreshOverlay();
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		shutting_down = true;
		if (overlay_server)
			overlay_server->close();
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
		   event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
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
			 [messages](const QString &name, const QString &message) {
				 messages->append(QString("<span style='color:#ff0033'>● YouTube</span> <b>%1:</b> %2")
							  .arg(name.toHtmlEscaped(), message.toHtmlEscaped()));
				 updateOverlay("YouTube", name, message);
			 });
	QObject::connect(kick, &KickClient::status, body,
			 [status](const QString &message) { status->setText(message); });
	QObject::connect(kick, &KickClient::message, body, [messages](const QString &name, const QString &message) {
		messages->append(QString("<span style='color:#53fc18'>● Kick</span> <b>%1:</b> %2")
					 .arg(name.toHtmlEscaped(), message.toHtmlEscaped()));
		updateOverlay("Kick", name, message);
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
	QObject::connect(socket, &QSslSocket::readyRead, body, [socket, buffer, messages]() {
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
			if (line.startsWith('@')) {
				QByteArray tags = line.mid(1, line.indexOf(' ') - 1);
				for (const auto &tag : tags.split(';'))
					if (tag.startsWith("display-name="))
						name = ircUnescape(QString::fromUtf8(tag.mid(13)));
			}
			if (name.isEmpty()) {
				int prefix = line.indexOf(" :");
				int bang = line.indexOf('!', prefix + 2);
				if (prefix >= 0 && bang > prefix)
					name = QString::fromUtf8(line.mid(prefix + 2, bang - prefix - 2));
			}
			if (name.isEmpty())
				continue;
			QString message = QString::fromUtf8(line.mid(content + 2)).toHtmlEscaped();
			messages->append(QString("<span style='color:#9146ff'>● Twitch</span> <b>%1:</b> %2")
						 .arg(name.toHtmlEscaped(), message));
			updateOverlay("Twitch", name, QString::fromUtf8(line.mid(content + 2)));
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
			kick->start(kickChannel(inputs[1]->text()));
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
	kick->start(kickChannel(inputs[1]->text()));
	youtube->start(youtubeVideoId(inputs[2]->text()));
	if (!channel->isEmpty())
		connectChat();
	QObject::connect(test, &QPushButton::clicked, body, [messages]() {
		messages->append(QString::fromUtf8(
			"<span style='color:#9146ff'>● Twitch</span> <b>exemplo:</b> Painel funcionando."));
		messages->append(QString::fromUtf8(
			"<span style='color:#ff0033'>● YouTube</span> <b>exemplo:</b> Mensagem de teste."));
		updateOverlay("Twitch", "exemplo", "Painel funcionando.");
		updateOverlay("YouTube", "exemplo", "Mensagem de teste.");
	});
	if (!obs_frontend_add_dock_by_id("zosma-multichat", "Multichat", body)) {
		delete body;
		overlay_checkbox = nullptr;
		overlay_status = nullptr;
		return false;
	}
	startOverlayServer(body);
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
	if (overlay_server)
		overlay_server->close();
	overlay_server = nullptr;
	overlay_clients.clear();
	if (dock_registered) {
		obs_frontend_remove_dock("zosma-multichat");
		dock_registered = false;
	}
}
