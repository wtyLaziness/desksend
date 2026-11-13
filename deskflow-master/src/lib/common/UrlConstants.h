/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

// important: this is used for settings paths on some platforms,
// and must not be a url. qt automatically converts this to reverse domain
// notation (rdn), e.g. org.deskflow
const auto kOrgDomain = QStringLiteral("deskflow.org");

const auto kUrlSourceQuery = QStringLiteral("source=gui");
const auto kUrlApp = QStringLiteral("https://%1").arg(kOrgDomain);
const auto kUrlHelp = QStringLiteral("https://github.com/wtyLaziness/desksend/issues");
const auto kUrlDownload = QStringLiteral("https://github.com/wtyLaziness/desksend-releases/releases/latest");
const auto kUrlWiki = QStringLiteral("https://github.com/wtyLaziness/desksend");
const auto kUrlUpdateCheck = QStringLiteral("https://api.github.com/repos/wtyLaziness/desksend-releases/releases/latest");

#if defined(Q_OS_LINUX)
const auto kUrlGnomeTrayFix = QStringLiteral("https://extensions.gnome.org/extension/615/appindicator-support/");
#endif
