#pragma once

#include <QWidget>
#include <functional>
#include <string>

// ABI of the browser-panel interface exported by OBS's obs-browser module.
// The browser plugin is resolved at runtime so builds do not depend on CEF.
class QCefCookieManager;
class QCefWidget : public QWidget {
	Q_OBJECT

public:
	using QWidget::QWidget;
	virtual void setURL(const std::string &url) = 0;
	virtual void setStartupScript(const std::string &script) = 0;
	virtual void allowAllPopups(bool allow) = 0;
	virtual void closeBrowser() = 0;
	virtual void reloadPage() = 0;
	virtual bool zoomPage(int direction) = 0;
	virtual void executeJavaScript(const std::string &script) = 0;

signals:
	void titleChanged(const QString &title);
	void urlChanged(const QString &url);
};

struct QCef {
	virtual ~QCef() = default;
	virtual bool init_browser() = 0;
	virtual bool initialized() = 0;
	virtual bool wait_for_browser_init() = 0;
	virtual QCefWidget *create_widget(QWidget *parent, const std::string &url,
					  QCefCookieManager *cookie_manager = nullptr) = 0;
	virtual QCefCookieManager *create_cookie_manager(const std::string &storage_path,
							 bool persist_session_cookies = false) = 0;
	virtual void *get_cookie_path(const std::string &storage_path) = 0;
	virtual void add_popup_whitelist_url(const std::string &url, QObject *obj) = 0;
	virtual void add_force_popup_url(const std::string &url, QObject *obj) = 0;
};
