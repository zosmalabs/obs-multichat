#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <thread>

class YouTubeClient : public QObject {
	Q_OBJECT
public:
	explicit YouTubeClient(QObject *parent = nullptr);
	~YouTubeClient() override;
	void start(const QString &videoId);
signals:
	void status(const QString &message);
	void message(const QString &name, const QString &text);

private:
	void stop();
	std::thread worker;
	std::atomic_bool cancelled{false};
};
