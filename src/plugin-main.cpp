#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QDockWidget>
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

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static QDockWidget *dock = nullptr;

static bool validSource(int platform, const QString &value)
{
	const QString input = value.trimmed();
	if (input.isEmpty())
		return true;
	if (platform == 0 || platform == 1 || platform == 3) {
		if (QRegularExpression("^@?[a-zA-Z0-9_.-]+$").match(input).hasMatch())
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
		return host == "twitch.tv" && url.path().contains('/');
	case 1:
		return host == "kick.com" && url.path().contains('/');
	case 2:
		return (host == "youtube.com" && (url.path().startsWith("/watch") || url.path().startsWith("/live/") ||
						  url.path().startsWith("/live_chat"))) ||
		       (host == "youtu.be" && url.path().size() > 1);
	case 3:
		return host == "tiktok.com" && url.path().startsWith("/@");
	}
	return false;
}

bool obs_module_load(void)
{
	// OBS calls this after the frontend is initialized; ownership of the dock
	// is transferred to the OBS main window on successful registration.
	dock = new QDockWidget(QString::fromUtf8("Multichat"));
	dock->setObjectName("zosma-multichat");
	auto *body = new QWidget(dock);
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
	auto *overlay = new QCheckBox(QString::fromUtf8("Exibir na transmissão (em desenvolvimento)"), body);
	overlay->setEnabled(false);
	layout->addWidget(overlay);
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
	auto *test = new QPushButton(QString::fromUtf8("Testar painel"), body);
	layout->addWidget(test);
	QObject::connect(save, &QPushButton::clicked, body, [inputs, status]() {
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
		status->setText(settings.status() == QSettings::NoError
					? QString::fromUtf8("Fontes salvas. Captura ainda não ativada.")
					: QString::fromUtf8("Falha ao salvar as fontes."));
	});
	QObject::connect(test, &QPushButton::clicked, body, [messages]() {
		messages->append(QString::fromUtf8(
			"<span style='color:#9146ff'>● Twitch</span> <b>exemplo:</b> Painel funcionando."));
		messages->append(QString::fromUtf8(
			"<span style='color:#ff0033'>● YouTube</span> <b>exemplo:</b> Mensagem de teste."));
	});
	dock->setWidget(body);
	if (!obs_frontend_add_custom_qdock("zosma-multichat", dock)) {
		delete dock;
		dock = nullptr;
		return false;
	}
	obs_log(LOG_INFO, "Multichat test dock loaded");
	return true;
}

void obs_module_unload(void)
{
	dock = nullptr; // OBS owns and destroys the registered dock.
}
