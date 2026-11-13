// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/StyleUtils.h"
#include "gui/widgets/LogWidget.h"

#include <QFontDatabase>
#include <QFontInfo>
#include <QPixmap>
#include <QtTest>

class StyleUtilsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void fixedFontResolvesToScalableFont()
  {
    const QFont font = fixedFont();
    const QFontInfo info(font);
    QVERIFY(!info.family().isEmpty());
#ifdef Q_OS_WIN
    QVERIFY2(
        info.family().compare(QStringLiteral("Fixedsys"), Qt::CaseInsensitive) != 0,
        qPrintable(QStringLiteral("Unexpected bitmap font fallback: %1").arg(info.family()))
    );
    QVERIFY2(
        QFontDatabase::isSmoothlyScalable(info.family()),
        qPrintable(QStringLiteral("Expected smoothly scalable font, got: %1").arg(info.family()))
    );
#endif
  }

  void logWidgetRendersMixedAsciiAndCjkLines()
  {
    LogWidget logWidget;
    logWidget.resize(640, 240);
    logWidget.appendLine(QStringLiteral("[2026-11-13T18:34:02.123] INFO: FileDropBridge: drop at (100,200)"));
    logWidget.appendLine(QStringLiteral("[2026-11-13T18:34:02.456] INFO: 正在传输 测试文件—副本 (1).zip"));
    const QPixmap rendered = logWidget.grab();
    QVERIFY(!rendered.isNull());
  }
};

QTEST_MAIN(StyleUtilsTests)
#include "StyleUtilsTests.moc"
