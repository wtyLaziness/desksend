/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreArgParser.h"

#include <QTest>

class CoreArgParserTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void parsesDropAgentFlagWithoutPositionalMode()
  {
    CoreArgParser parser({QStringLiteral("deskflow-core"), QStringLiteral("--drop-agent")});
    QVERIFY(parser.dropAgentMode());
    QVERIFY(!parser.help());
    QVERIFY(!parser.version());
    QVERIFY(parser.errorText().isEmpty());

    parser.parse();
    QVERIFY(!parser.serverMode());
    QVERIFY(!parser.clientMode());
  }

  void parsesStandardServerAndClientModes()
  {
    CoreArgParser serverParser({QStringLiteral("deskflow-core"), QStringLiteral("server")});
    QVERIFY(!serverParser.dropAgentMode());
    serverParser.parse();
    QVERIFY(serverParser.serverMode());
    QVERIFY(!serverParser.clientMode());

    CoreArgParser clientParser({QStringLiteral("deskflow-core"), QStringLiteral("client")});
    QVERIFY(!clientParser.dropAgentMode());
    clientParser.parse();
    QVERIFY(!clientParser.serverMode());
    QVERIFY(clientParser.clientMode());
  }
};

QTEST_MAIN(CoreArgParserTests)
#include "CoreArgParserTests.moc"
