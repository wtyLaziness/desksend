/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AboutDialog.h"
#include "ui_AboutDialog.h"

#include "common/Constants.h"
#include "common/Settings.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"

#include <QClipboard>

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent), ui{std::make_unique<Ui::AboutDialog>()}
{
  ui->setupUi(this);

  const int px = (fontMetrics().height() * 6);
  const QSize pixmapSize(px, px);
  ui->lblIcon->setFixedSize(pixmapSize);

  ui->lblIcon->setPixmap(QPixmap(QIcon(":/branding/desksend.png").pixmap(QSize().scaled(pixmapSize, Qt::KeepAspectRatio)))
  );

  ui->btnCopyVersion->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::EditCopy));
  ui->btnCopyVersion->setAccessibleName(tr("Copy version info"));
  connect(ui->btnCopyVersion, &QPushButton::clicked, this, &AboutDialog::copyVersionText);

  ui->lblVersion->setText(kIsPreviewBuild ? tr("%1 (preview)").arg(kDisplayVersion) : QString::fromLatin1(kDisplayVersion));
  ui->lblDescription->setText(
      tr("Use one keyboard and mouse to work across multiple computers.") + QLatin1Char('\n') +
      tr("Drag files between screens to send them. Either side can cancel an active transfer.")
  );
  ui->lblProjectLinks->setText(
      QStringLiteral("<a href=\"%1\">%2</a> · <a href=\"%3\">%4</a> · <a href=\"%5\">%6</a>")
          .arg(kUrlWiki, tr("Source code"), kUrlUserGuide, tr("User guide"), kUrlDownload, tr("Releases and downloads"))
  );
  ui->lblMaintainer->setText(
      QStringLiteral("%1 <a href=\"%2/graphs/contributors\">%3</a>")
          .arg(
              tr("Maintained by %1.").arg(QStringLiteral("<a href=\"https://github.com/wtyLaziness\">wtyLaziness</a>")),
              kUrlWiki, tr("View project contributors")
          )
  );
  ui->lblUpstreamProjects->setText(
      QStringLiteral("<p style=\"margin:0 0 6px\">%1<br>%2</p><p style=\"margin:0 0 6px\">%3<br>%4</p>"
                     "<a href=\"%5/blob/main/LICENSE.md\">%6</a>")
          .arg(
              tr("Keyboard and mouse sharing is based on %1. Local file transfers use %2.")
                  .arg(
                      QStringLiteral("<a href=\"https://github.com/deskflow/deskflow\">Deskflow</a>"),
                      QStringLiteral("<a href=\"https://github.com/localsend/localsend\">LocalSend</a>")
                  ),
              tr("Some interface icons come from %1.")
                  .arg(QStringLiteral("<a href=\"https://develop.kde.org/frameworks/breeze-icons/\">KDE Breeze</a>")),
              tr("The Deskflow components use GPL v2 with the OpenSSL linking exception."),
              tr("The LocalSend components use Apache License 2.0."),
              kUrlWiki, tr("View licenses")
          )
  );
  ui->lblCopyright->setText(kCopyright);
  ui->lblDeskflowContributors->setText(
      QStringLiteral("<a href=\"https://github.com/deskflow/deskflow/graphs/contributors\">%1</a><br>%2")
          .arg(tr("Deskflow upstream contributors"), s_deskflowContributors.join(QStringLiteral(", ")).toHtmlEscaped())
  );
  ui->upstreamDetails->hide();
  resize(width(), sizeHint().height());
  connect(ui->btnUpstreamDetails, &QPushButton::toggled, this, [this](bool expanded) {
    ui->upstreamDetails->setVisible(expanded);
    ui->btnUpstreamDetails->setText(
        expanded ? tr("Hide upstream copyright and contributors") : tr("Show upstream copyright and contributors")
    );
    resize(width(), sizeHint().height());
  });

  ui->btnOk->setDefault(true);
  connect(ui->btnOk, &QPushButton::clicked, this, &AboutDialog::close);
}

void AboutDialog::copyVersionText() const
{
  QString infoString = QStringLiteral("%1: %2").arg(kAppName, kVersion);
  const auto revision = QString::fromLatin1(kVersionGitSha);
  if (!revision.isEmpty()) {
    infoString.append(QStringLiteral(" (%1)").arg(revision));
  }
  if (kIsPreviewBuild) {
    infoString.append(QStringLiteral("\nPreview build"));
  }
  infoString.append(QStringLiteral("\nQt: %1\nSystem: %2").arg(qVersion(), QSysInfo::prettyProductName()));
  if (Settings::isPortableMode()) {
    infoString.append(QStringLiteral("\nPortable Mode"));
  }
#ifdef Q_OS_LINUX
  infoString.append(QStringLiteral("\nSession: %1 (%2)")
                        .arg(qEnvironmentVariable("XDG_CURRENT_DESKTOP"), qEnvironmentVariable("XDG_SESSION_TYPE")));
#endif
  QGuiApplication::clipboard()->setText(infoString);
}

AboutDialog::~AboutDialog() = default;
