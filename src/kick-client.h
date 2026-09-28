#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <thread>

class KickClient : public QObject {
	Q_OBJECT
public:
	explicit KickClient(QObject *parent = nullptr);
	~KickClient() override;
	void start(const QString &channel);
signals:
	void status(const QString &message);
	void message(const QString &name, const QString &text);

private:
	void stop();
	std::thread worker;
	std::atomic_bool cancelled{false};
};
