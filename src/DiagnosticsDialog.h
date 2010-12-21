/*
 * QEstEidUtil
 *
 * Copyright (C) 2009,2010 Jargo Kõster <jargo@innovaatik.ee>
 * Copyright (C) 2009,2010 Raul Metsma <raul@innovaatik.ee>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "ui_DiagnosticsDialog.h"

#include <smartcardpp/common.h>

#if defined(Q_OS_WIN32)
#include <Windows.h>
#endif

class DiagnosticsDialog: public QDialog, private Ui::DiagnosticsDialog
{
	Q_OBJECT
public:
	DiagnosticsDialog( QWidget *parent = 0 );

#if defined(Q_OS_WIN32)
	static bool addCert( HCERTSTORE store, ByteVec &cert, const QString &card, DWORD keyCode );
	static QString checkCert( ByteVec &bytes, ByteVec &certBytesSign, const QString &cardId );
#endif

private slots:
	void save();

private:
	bool	isPCSCRunning() const;
	QString getBrowsers() const;
	QString getPackageVersion( const QStringList &list, bool returnPackageName = true ) const;
	QString getProcessor() const;
	QString getReaderInfo();
#if defined(Q_OS_WIN32)
	QString getBits() const;
	QString getLibVersion( const QString &lib ) const;
	QString getOS() const;

	QString certInfo;
#endif
};
