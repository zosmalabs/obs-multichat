#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
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
static obs_source_t *overlay_source = nullptr;
static QCheckBox *overlay_checkbox = nullptr;
static QLabel *overlay_status = nullptr;
static QStringList overlay_lines;

static bool attachOverlay()
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return false;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene) {
		obs_source_release(scene_source);
		return false;
	}
	if (!overlay_source)
		overlay_source = obs_get_source_by_name("Zosma Multichat");
	if (!overlay_source) {
		obs_data_t *font = obs_data_create();
		obs_data_set_string(font, "face", "Arial");
		obs_data_set_int(font, "size", 44);
		obs_data_t *settings = obs_data_create();
		obs_data_set_obj(settings, "font", font);
		obs_data_set_string(settings, "text", "Multichat pronto. Aguardando mensagens...");
		obs_data_set_bool(settings, "outline", true);
#ifdef _WIN32
		const char *source_id = "text_gdiplus";
#else
		const char *source_id = "text_ft2_source";
#endif
		overlay_source = obs_source_create(source_id, "Zosma Multichat", settings, nullptr);
		obs_data_release(settings);
		obs_data_release(font);
	}
	if (overlay_source && !obs_scene_find_source(scene, "Zosma Multichat"))
		obs_scene_add(scene, overlay_source);
	if (overlay_source)
		obs_source_set_enabled(overlay_source, true);
	obs_source_release(scene_source);
	return overlay_source != nullptr;
}

static void updateOverlay(const QString &platform, const QString &name, const QString &message)
{
	QString text = QString("%1 · %2: %3").arg(platform, name, message);
	text.replace('\n', ' ').replace('\r', ' ');
	overlay_lines.append(text.left(200));
	while (overlay_lines.size() > 8)
		overlay_lines.removeFirst();
	if (overlay_source && overlay_checkbox && overlay_checkbox->isChecked()) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "text", overlay_lines.join('\n').toUtf8().constData());
		obs_source_update(overlay_source, settings);
		obs_data_release(settings);
	}
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		if (overlay_source)
			obs_source_release(overlay_source);
		overlay_source = nullptr;
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
		   event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		if (overlay_checkbox && overlay_checkbox->isChecked() && attachOverlay() && overlay_status)
			overlay_status->setText(QString::fromUtf8("Fonte Zosma Multichat ativa na cena atual."));
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
	overlay_status = new QLabel(QString::fromUtf8("Fonte de texto nas cenas usadas durante a live."), body);
	overlay_status->setWordWrap(true);
	layout->addWidget(overlay_status);
	QObject::connect(overlay, &QCheckBox::toggled, body, [](bool enabled) {
		QSettings settings("Zosma", "OBS Multichat");
		settings.setValue("overlay", enabled);
		if (enabled) {
			if (overlay_status)
				overlay_status->setText(
					attachOverlay()
						? QString::fromUtf8("Fonte Zosma Multichat ativa na cena atual.")
						: QString::fromUtf8(
							  "Não foi possível adicionar a fonte à cena atual."));
		} else {
			if (overlay_source)
				obs_source_set_enabled(overlay_source, false);
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
	layout->addWidget(messages);
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
	dock_registered = true;
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_log(LOG_INFO, "Multichat test dock loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	if (overlay_source)
		obs_source_release(overlay_source);
	overlay_source = nullptr;
	overlay_checkbox = nullptr;
	overlay_status = nullptr;
	overlay_lines.clear();
	if (dock_registered) {
		obs_frontend_remove_dock("zosma-multichat");
		dock_registered = false;
	}
}
