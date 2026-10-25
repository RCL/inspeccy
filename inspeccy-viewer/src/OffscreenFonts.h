#pragma once
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QStandardPaths>

// Unlike the native Windows platform plugin, Qt's offscreen plugin may start
// with an empty font database. Register installed fonts for readable PNGs;
// no font assets are bundled and interactive application styling is untouched.
inline void ConfigureOffscreenFonts()
{
#ifdef Q_OS_WIN
	if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
	{
		return;
	}
	const auto Locations = QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
	bool HaveUiFont = false;
	for (const auto& Location : Locations)
	{
		for (const auto* Name : {"segoeui.ttf", "segoeuib.ttf", "consola.ttf", "consolab.ttf"})
		{
			const int Id = QFontDatabase::addApplicationFont(QDir(Location).filePath(QString::fromLatin1(Name)));
			if (Id >= 0 && QByteArray(Name) == "segoeui.ttf")
			{
				HaveUiFont = true;
			}
		}
	}
	if (HaveUiFont)
	{
		QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 10));
	}
#endif
}
