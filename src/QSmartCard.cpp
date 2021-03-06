/*
 * QEstEidUtil
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

#include "QSmartCard_p.h"

#include <common/IKValidator.h>
#include <common/PinDialog.h>
#include <common/Settings.h>

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QScopedPointer>
#include <QtNetwork/QSslKey>
#include <QtWidgets/QApplication>

#include <openssl/obj_mac.h>
#include <thread>

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static int ECDSA_SIG_set0(ECDSA_SIG *sig, BIGNUM *r, BIGNUM *s)
{
	if(!r || !s)
		return 0;
	BN_clear_free(sig->r);
	BN_clear_free(sig->s);
	sig->r = r;
	sig->s = s;
	return 1;
}
#endif

QSmartCardData::QSmartCardData(): d(new QSmartCardDataPrivate) {}
QSmartCardData::QSmartCardData(const QSmartCardData &other) = default;
QSmartCardData::QSmartCardData(QSmartCardData &&other) Q_DECL_NOEXCEPT: d(std::move(other.d)) {}
QSmartCardData::~QSmartCardData() = default;
QSmartCardData& QSmartCardData::operator =(const QSmartCardData &other) = default;
QSmartCardData& QSmartCardData::operator =(QSmartCardData &&other) Q_DECL_NOEXCEPT { qSwap(d, other.d); return *this; }

QString QSmartCardData::card() const { return d->card; }
QStringList QSmartCardData::cards() const { return d->cards; }

bool QSmartCardData::isNull() const
{ return d->data.isEmpty() && d->authCert.isNull() && d->signCert.isNull(); }
bool QSmartCardData::isPinpad() const { return d->pinpad; }
bool QSmartCardData::isSecurePinpad() const
{ return d->reader.contains(QLatin1String("EZIO SHIELD"), Qt::CaseInsensitive); }
bool QSmartCardData::isValid() const
{ return d->data.value(Expiry).toDateTime() >= QDateTime::currentDateTime(); }

QString QSmartCardData::reader() const { return d->reader; }
QStringList QSmartCardData::readers() const { return d->readers; }

QVariant QSmartCardData::data(PersonalDataType type) const
{ return d->data.value(type); }
SslCertificate QSmartCardData::authCert() const { return d->authCert; }
SslCertificate QSmartCardData::signCert() const { return d->signCert; }
quint8 QSmartCardData::retryCount(PinType type) const { return d->retry.value(type); }
ulong QSmartCardData::usageCount(PinType type) const { return d->usage.value(type); }
QString QSmartCardData::appletVersion() const { return d->appletVersion; }
QSmartCardData::CardVersion QSmartCardData::version() const { return d->version; }

quint8 QSmartCardData::minPinLen(QSmartCardData::PinType type)
{
	switch(type)
	{
	case QSmartCardData::Pin1Type: return 4;
	case QSmartCardData::Pin2Type: return 5;
	case QSmartCardData::PukType: return 8;
	}
}

QString QSmartCardData::typeString(QSmartCardData::PinType type)
{
	switch(type)
	{
	case Pin1Type: return QStringLiteral("PIN1");
	case Pin2Type: return QStringLiteral("PIN2");
	case PukType: return QStringLiteral("PUK");
	}
	return QString();
}



QSharedPointer<QPCSCReader> QSmartCard::Private::connect(const QString &reader)
{
	qDebug() << "Connecting to reader" << reader;
	QSharedPointer<QPCSCReader> r(new QPCSCReader(reader, &QPCSC::instance()));
	if(!r->connect() || !r->beginTransaction())
		r.clear();
	return r;
}

QSmartCard::ErrorType QSmartCard::Private::handlePinResult(QPCSCReader *reader, const QPCSCReader::Result &response, bool forceUpdate)
{
	if(!response || forceUpdate)
		updateCounters(reader, t.d);
	switch((quint8(response.SW[0]) << 8) + quint8(response.SW[1]))
	{
	case 0x9000: return QSmartCard::NoError;
	case 0x63C0: return QSmartCard::BlockedError;//pin retry count 0
	case 0x63C1: // Validate error, 1 tries left
	case 0x63C2: // Validate error, 2 tries left
	case 0x63C3: return QSmartCard::ValidateError;
	case 0x6400: // Timeout (SCM)
	case 0x6401: return QSmartCard::CancelError; // Cancel (OK, SCM)
	case 0x6402: return QSmartCard::DifferentError;
	case 0x6403: return QSmartCard::LenghtError;
	case 0x6983: return QSmartCard::BlockedError;
	case 0x6985: return QSmartCard::OldNewPinSameError;
	case 0x6A80: return QSmartCard::OldNewPinSameError;
	default: return QSmartCard::UnknownError;
	}
}

quint16 QSmartCard::Private::language() const
{
	if(Settings().language() == QLatin1String("en")) return 0x0409;
	if(Settings().language() == QLatin1String("et")) return 0x0425;
	if(Settings().language() == QLatin1String("ru")) return 0x0419;
	return 0x0000;
}

QByteArray QSmartCard::Private::sign(const QByteArray &dgst, Private *d)
{
	if(!d ||
		!d->reader ||
		!d->reader->transfer(d->SECENV1) ||
		!d->reader->transfer(APDU("002241B8 02 8300"))) //Key reference, 8303801100
		return QByteArray();
	QByteArray cmd = APDU("0088000000"); //calc signature
	cmd[4] = char(dgst.size());
	cmd += dgst;
	QPCSCReader::Result result = d->reader->transfer(cmd);
	if(!result)
		return QByteArray();
	return result.data;
}

int QSmartCard::Private::rsa_sign(int type, const unsigned char *m, unsigned int m_len,
		unsigned char *sigret, unsigned int *siglen, const RSA *rsa)
{
	QByteArray data;
	switch(type)
	{
	case NID_sha1: data += QByteArray::fromHex("3021300906052b0e03021a05000414"); break;
	case NID_sha224: data += QByteArray::fromHex("302d300d06096086480165030402040500041c"); break;
	case NID_sha256: data += QByteArray::fromHex("3031300d060960864801650304020105000420"); break;
	case NID_sha384: data += QByteArray::fromHex("3041300d060960864801650304020205000430"); break;
	case NID_sha512: data += QByteArray::fromHex("3051300d060960864801650304020305000440"); break;
	default: break;
	}
	data += QByteArray::fromRawData((const char*)m, int(m_len));
	QByteArray result = sign(data, (Private*)RSA_get_app_data(rsa));
	if(result.isEmpty())
		return 0;
	*siglen = (unsigned int)result.size();
	memcpy(sigret, result.constData(), size_t(result.size()));
	return 1;
}

ECDSA_SIG* QSmartCard::Private::ecdsa_do_sign(const unsigned char *dgst, int dgst_len,
		const BIGNUM *, const BIGNUM *, EC_KEY *eckey)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	Private *d = (Private*)ECDSA_get_ex_data(eckey, 0);
#else
	Private *d = (Private*)EC_KEY_get_ex_data(eckey, 0);
#endif
	QByteArray result = sign(QByteArray::fromRawData((const char*)dgst, dgst_len), d);
	if(result.isEmpty())
		return nullptr;
	QByteArray r = result.left(result.size()/2);
	QByteArray s = result.right(result.size()/2);
	ECDSA_SIG *sig = ECDSA_SIG_new();
	ECDSA_SIG_set0(sig,
		BN_bin2bn((const unsigned char*)r.data(), int(r.size()), nullptr),
		BN_bin2bn((const unsigned char*)s.data(), int(s.size()), nullptr));
	return sig;
}

bool QSmartCard::Private::updateCounters(QPCSCReader *reader, QSmartCardDataPrivate *d)
{
	if(!reader->transfer(MASTER_FILE) ||
		!reader->transfer(PINRETRY))
		return false;

	QByteArray cmd = READRECORD;
	for(int i = QSmartCardData::Pin1Type; i <= QSmartCardData::PukType; ++i)
	{
		cmd[2] = char(i);
		QPCSCReader::Result data = reader->transfer(cmd);
		if(!data)
			return false;
		d->retry[QSmartCardData::PinType(i)] = quint8(data.data[5]);
	}

	if(!reader->transfer(ESTEIDDF) ||
		!reader->transfer(KEYPOINTER))
		return false;

	cmd[2] = 1;
	QPCSCReader::Result data = reader->transfer(cmd);
	if(!data)
		return false;

	/*
	 * SIGN1 0100 1
	 * SIGN2 0200 2
	 * AUTH1 1100 3
	 * AUTH2 1200 4
	 */
	quint8 signkey = data.data.at(0x13) == 0x01 && data.data.at(0x14) == 0x00 ? 1 : 2;
	quint8 authkey = data.data.at(0x09) == 0x11 && data.data.at(0x0A) == 0x00 ? 3 : 4;

	if(!reader->transfer(KEYUSAGE))
		return false;

	cmd[2] = char(authkey);
	data = reader->transfer(cmd);
	if(!data)
		return false;
	d->usage[QSmartCardData::Pin1Type] = 0xFFFFFF - ((quint8(data.data[12]) << 16) + (quint8(data.data[13]) << 8) + quint8(data.data[14]));

	cmd[2] = char(signkey);
	data = reader->transfer(cmd);
	if(!data)
		return false;
	d->usage[QSmartCardData::Pin2Type] = 0xFFFFFF - ((quint8(data.data[12]) << 16) + (quint8(data.data[13]) << 8) + quint8(data.data[14]));
	return true;
}




QSmartCard::QSmartCard(QObject *parent)
:	QThread(parent)
,	d(new Private)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	d->rsamethod.name = "QSmartCard";
	d->rsamethod.rsa_sign = Private::rsa_sign;
	ECDSA_METHOD_set_name(d->ecmethod, const_cast<char*>("QSmartCard"));
	ECDSA_METHOD_set_sign(d->ecmethod, Private::ecdsa_do_sign);
	ECDSA_METHOD_set_app_data(d->ecmethod, d);
#else
	RSA_meth_set1_name(d->rsamethod, "QSmartCard");
	RSA_meth_set_sign(d->rsamethod, Private::rsa_sign);
	typedef int (*EC_KEY_sign)(int type, const unsigned char *dgst, int dlen, unsigned char *sig,
		unsigned int *siglen, const BIGNUM *kinv, const BIGNUM *r, EC_KEY *eckey);
	typedef int (*EC_KEY_sign_setup)(EC_KEY *eckey, BN_CTX *ctx_in, BIGNUM **kinvp, BIGNUM **rp);
	EC_KEY_sign sign = nullptr;
	EC_KEY_sign_setup sign_setup = nullptr;
	EC_KEY_METHOD_get_sign(d->ecmethod, &sign, &sign_setup, nullptr);
	EC_KEY_METHOD_set_sign(d->ecmethod, sign, sign_setup, Private::ecdsa_do_sign);
#endif

	d->t.d->readers = QPCSC::instance().readers();
	d->t.d->card = QStringLiteral("loading");
	d->t.d->cards = QStringList() << d->t.d->card;
}

QSmartCard::~QSmartCard()
{
	requestInterruption();
	wait();
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	RSA_meth_free(d->rsamethod);
	EC_KEY_METHOD_free(d->ecmethod);
#else
	ECDSA_METHOD_free(d->ecmethod);
#endif
	delete d;
}

QSmartCard::ErrorType QSmartCard::change(QSmartCardData::PinType type, const QString &newpin, const QString &pin)
{
	QMutexLocker locker(&d->m);
	QSharedPointer<QPCSCReader> reader(d->connect(d->t.reader()));
	if(!reader)
		return UnknownError;
	QByteArray cmd = d->CHANGE;
	cmd[3] = type == QSmartCardData::PukType ? 0 : type;
	cmd[4] = char(pin.size() + newpin.size());
	QPCSCReader::Result result;
	if(d->t.isPinpad())
	{
		QEventLoop l;
		std::thread([&]{
			result = reader->transferCTL(cmd, false, d->language(), QSmartCardData::minPinLen(type));
			l.quit();
		}).detach();
		l.exec();
	}
	else
		result = reader->transfer(cmd + pin.toUtf8() + newpin.toUtf8());
	return d->handlePinResult(reader.data(), result, true);
}

QSmartCardData QSmartCard::data() const { return d->t; }

QSslKey QSmartCard::key() const
{
	QSslKey key = d->t.authCert().publicKey();
	if(!key.handle())
		return key;
	if (key.algorithm() == QSsl::Ec)
	{
		EC_KEY *ec = (EC_KEY*)key.handle();
#if OPENSSL_VERSION_NUMBER < 0x10100000L
		ECDSA_set_ex_data(ec, 0, d);
		ECDSA_set_method(ec, d->ecmethod);
#else
		EC_KEY_set_ex_data(ec, 0, d);
		EC_KEY_set_method(ec, d->ecmethod);
#endif
	}
	else
	{
		RSA *rsa = (RSA*)key.handle();
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
		RSA_set_method(rsa, &d->rsamethod);
		rsa->flags |= RSA_FLAG_SIGN_VER;
#else
		RSA_set_method(rsa, d->rsamethod);
#endif
		RSA_set_app_data(rsa, d);
	}
	return key;
}

QSmartCard::ErrorType QSmartCard::login(QSmartCardData::PinType type)
{
	PinDialog::PinFlags flags = PinDialog::Pin1Type;
	QSslCertificate cert;
	switch(type)
	{
	case QSmartCardData::Pin1Type: flags = PinDialog::Pin1Type; cert = d->t.authCert(); break;
	case QSmartCardData::Pin2Type: flags = PinDialog::Pin2Type; cert = d->t.signCert(); break;
	default: return UnknownError;
	}

	QScopedPointer<PinDialog> p;
	QByteArray pin;
	if(!d->t.isPinpad())
	{
		p.reset(new PinDialog(flags, cert, nullptr, qApp->activeWindow()));
		if(!p->exec())
			return CancelError;
		pin = p->text().toUtf8();
	}
	else
		p.reset(new PinDialog(PinDialog::PinFlags(flags|PinDialog::PinpadFlag), cert, nullptr, qApp->activeWindow()));

	d->m.lock();
	d->reader = d->connect(d->t.reader());
	if(!d->reader)
	{
		d->m.unlock();
		return UnknownError;
	}
	QByteArray cmd = d->VERIFY;
	cmd[3] = type;
	cmd[4] = char(pin.size());
	QPCSCReader::Result result;
	if(d->t.isPinpad())
	{
		std::thread([&]{
			Q_EMIT p->startTimer();
			result = d->reader->transferCTL(cmd, true, d->language(), QSmartCardData::minPinLen(type));
			Q_EMIT p->finish(0);
		}).detach();
		p->exec();
	}
	else
		result = d->reader->transfer(cmd + pin);
	QSmartCard::ErrorType err = d->handlePinResult(d->reader.data(), result, false);
	if(!result)
	{
		d->updateCounters(d->reader.data(), d->t.d);
		d->reader.clear();
		d->m.unlock();
	}
	return err;
}

void QSmartCard::logout()
{
	if(d->reader.isNull())
		return;
	d->updateCounters(d->reader.data(), d->t.d);
	d->reader.clear();
	d->m.unlock();
}

QHash<quint8,QByteArray> QSmartCard::parseFCI(const QByteArray &data)
{
	QHash<quint8,QByteArray> result;
	for(QByteArray::const_iterator i = data.constBegin(); i != data.constEnd(); ++i)
	{
		quint8 tag(*i), size(*++i);
		result[tag] = QByteArray(i + 1, size);
		switch(tag)
		{
		case 0x6F:
		case 0x62:
		case 0x64:
		case 0xA1: continue;
		default: i += size; break;
		}
	}
	return result;
}

void QSmartCard::reload() { selectCard(d->t.card());  }

void QSmartCard::run()
{
	static const QHash<QByteArray,QSmartCardData::CardVersion> atrList{
		{"3BFE9400FF80B1FA451F034573744549442076657220312E3043", QSmartCardData::VER_1_0}, /*ESTEID_V1_COLD_ATR*/
		{"3B6E00FF4573744549442076657220312E30", QSmartCardData::VER_1_0}, /*ESTEID_V1_WARM_ATR*/
		{"3BDE18FFC080B1FE451F034573744549442076657220312E302B", QSmartCardData::VER_1_0_2007}, /*ESTEID_V1_2007_COLD_ATR*/
		{"3B5E11FF4573744549442076657220312E30", QSmartCardData::VER_1_0_2007}, /*ESTEID_V1_2007_WARM_ATR*/
		{"3B6E00004573744549442076657220312E30", QSmartCardData::VER_1_1}, /*ESTEID_V1_1_COLD_ATR*/
		{"3BFE1800008031FE454573744549442076657220312E30A8", QSmartCardData::VER_3_4}, /*ESTEID_V3_COLD_DEV1_ATR*/
		{"3BFE1800008031FE45803180664090A4561B168301900086", QSmartCardData::VER_3_4}, /*ESTEID_V3_WARM_DEV1_ATR*/
		{"3BFE1800008031FE45803180664090A4162A0083019000E1", QSmartCardData::VER_3_4}, /*ESTEID_V3_WARM_DEV2_ATR*/
		{"3BFE1800008031FE45803180664090A4162A00830F9000EF", QSmartCardData::VER_3_4}, /*ESTEID_V3_WARM_DEV3_ATR*/
		{"3BF9180000C00A31FE4553462D3443432D303181", QSmartCardData::VER_3_5}, /*ESTEID_V35_COLD_DEV1_ATR*/
		{"3BF81300008131FE454A434F5076323431B7", QSmartCardData::VER_3_5}, /*ESTEID_V35_COLD_DEV2_ATR*/
		{"3BFA1800008031FE45FE654944202F20504B4903", QSmartCardData::VER_3_5}, /*ESTEID_V35_COLD_DEV3_ATR*/
		{"3BFE1800008031FE45803180664090A4162A00830F9000EF", QSmartCardData::VER_3_5}, /*ESTEID_V35_WARM_ATR*/
		{"3BFE1800008031FE45803180664090A5102E03830F9000EF", QSmartCardData::VER_3_5}, /*UPDATER_TEST_CARDS*/
	};

	QByteArray cardid = d->READRECORD;
	cardid[2] = 8;

	while(!isInterruptionRequested())
	{
		if(d->m.tryLock())
		{
			// Get list of available cards
			QMap<QString,QString> cards;
			const QStringList readers = QPCSC::instance().readers();
			if(![&] {
				for(const QString &name: readers)
				{
					qDebug() << "Connecting to reader" << name;
					QScopedPointer<QPCSCReader> reader(new QPCSCReader(name, &QPCSC::instance()));
					if(!reader->isPresent())
						continue;

					if(!atrList.contains(reader->atr()))
					{
						qDebug() << "Unknown ATR" << reader->atr();
						continue;
					}

					switch(reader->connectEx())
					{
					case 0x8010000CL: continue; //SCARD_E_NO_SMARTCARD
					case 0:
						if(reader->beginTransaction())
							break;
					default: return false;
					}

					QPCSCReader::Result result;
					#define TRANSFERIFNOT(X) result = reader->transfer(X); \
						if(result.err) return false; \
						if(!result)

					TRANSFERIFNOT(d->MASTER_FILE)
					{	// Master file selection failed, test if it is updater applet
						TRANSFERIFNOT(d->UPDATER_AID)
							continue; // Updater applet not found
						TRANSFERIFNOT(d->MASTER_FILE)
						{	//Found updater applet but cannot select master file, select back 3.5
							reader->transfer(d->AID35);
							continue;
						}
					}
					TRANSFERIFNOT(d->ESTEIDDF)
						continue;
					TRANSFERIFNOT(d->PERSONALDATA)
						continue;
					TRANSFERIFNOT(cardid)
						continue;
					QString nr = d->codec->toUnicode(result.data);
					if(!nr.isEmpty())
						cards[nr] = name;
				}
				return true;
			}())
			{
				qDebug() << "Failed to poll card, try again next round";
				d->m.unlock();
				sleep(5);
				continue;
			}

			// cardlist has changed
			QStringList order = cards.keys();
			std::sort(order.begin(), order.end(), TokenData::cardsOrder);
			bool update = d->t.cards() != order || d->t.readers() != readers;

			// check if selected card is still in slot
			if(!d->t.card().isEmpty() && !order.contains(d->t.card()))
			{
				update = true;
				d->t.d = new QSmartCardDataPrivate();
			}

			d->t.d->cards = order;
			d->t.d->readers = readers;

			// if none is selected select first from cardlist
			if(d->t.card().isEmpty() && !d->t.cards().isEmpty())
			{
				QSharedDataPointer<QSmartCardDataPrivate> t = d->t.d;
				t->card = d->t.cards().first();
				t->data.clear();
				t->appletVersion.clear();
				t->authCert = QSslCertificate();
				t->signCert = QSslCertificate();
				d->t.d = t;
				update = true;
				Q_EMIT dataChanged();
			}

			// read card data
			if(d->t.cards().contains(d->t.card()) && d->t.isNull())
			{
				update = true;
				QSharedPointer<QPCSCReader> reader(d->connect(cards.value(d->t.card())));
				if(!reader.isNull())
				{
					QSharedDataPointer<QSmartCardDataPrivate> t = d->t.d;
					t->reader = reader->name();
					t->pinpad = reader->isPinPad();
					t->version = atrList.value(reader->atr(), QSmartCardData::VER_INVALID);
					if(t->version > QSmartCardData::VER_1_1)
					{
						if(reader->transfer(d->AID30).resultOk())
							t->version = QSmartCardData::VER_3_0;
						else if(reader->transfer(d->AID34).resultOk())
							t->version = QSmartCardData::VER_3_4;
						else if(reader->transfer(d->UPDATER_AID).resultOk())
						{
							t->version = QSmartCardData::CardVersion(t->version|QSmartCardData::VER_HASUPDATER);
							//Prefer EstEID applet when if it is usable
							if(!reader->transfer(d->AID35) ||
								!reader->transfer(d->MASTER_FILE))
							{
								reader->transfer(d->UPDATER_AID);
								t->version = QSmartCardData::VER_USABLEUPDATER;
							}
						}
					}

					bool tryAgain = !d->updateCounters(reader.data(), t);
					if(reader->transfer(d->PERSONALDATA).resultOk())
					{
						QByteArray cmd = d->READRECORD;
						for(int data = QSmartCardData::SurName; data != QSmartCardData::Comment4; ++data)
						{
							cmd[2] = char(data + 1);
							QPCSCReader::Result result = reader->transfer(cmd);
							if(!result)
							{
								tryAgain = true;
								break;
							}
							QString record = d->codec->toUnicode(result.data.trimmed());
							if(record == QChar(0))
								record.clear();
							switch(data)
							{
							case QSmartCardData::BirthDate:
							case QSmartCardData::Expiry:
							case QSmartCardData::IssueDate:
								t->data[QSmartCardData::PersonalDataType(data)] = QDate::fromString(record, QStringLiteral("dd.MM.yyyy"));
								break;
							default:
								t->data[QSmartCardData::PersonalDataType(data)] = record;
								break;
							}
						}
					}

					auto readCert = [&](const QByteArray &file) {
						QPCSCReader::Result data = reader->transfer(file + APDU(reader->protocol() == QPCSCReader::T1 ? "00" : ""));
						if(!data)
							return QSslCertificate();
						QHash<quint8,QByteArray> fci = parseFCI(data.data);
						int size = fci.contains(0x85) ? fci[0x85][0] << 8 | fci[0x85][1] : 0x0600;
						QByteArray cert;
						while(cert.size() < size)
						{
							QByteArray cmd = d->READBINARY;
							cmd[2] = char(cert.size() >> 8);
							cmd[3] = char(cert.size());
							data = reader->transfer(cmd);
							if(!data)
							{
								tryAgain = true;
								return QSslCertificate();
							}
							cert += data.data;
						}
						return QSslCertificate(cert, QSsl::Der);
					};
					t->authCert = readCert(d->AUTHCERT);
					t->signCert = readCert(d->SIGNCERT);

					QPCSCReader::Result data = reader->transfer(d->APPLETVER);
					if (data.resultOk())
					{
						for(int i = 0; i < data.data.size(); ++i)
						{
							if(i == 0)
								t->appletVersion = QString::number(quint8(data.data[i]));
							else
								t->appletVersion += QString(QStringLiteral(".%1")).arg(quint8(data.data[i]));
						}
					}

					t->data[QSmartCardData::Email] = t->authCert.subjectAlternativeNames().values(QSsl::EmailEntry).value(0);
					if(t->authCert.type() & SslCertificate::DigiIDType)
					{
						t->data[QSmartCardData::SurName] = t->authCert.toString(QStringLiteral("SN"));
						t->data[QSmartCardData::FirstName1] = t->authCert.toString(QStringLiteral("GN"));
						t->data[QSmartCardData::FirstName2] = QString();
						t->data[QSmartCardData::Id] = t->authCert.subjectInfo("serialNumber");
						t->data[QSmartCardData::BirthDate] = IKValidator::birthDate(t->authCert.subjectInfo("serialNumber"));
						t->data[QSmartCardData::IssueDate] = t->authCert.effectiveDate();
						t->data[QSmartCardData::Expiry] = t->authCert.expiryDate();
					}
					if(tryAgain)
					{
						qDebug() << "Failed to read card info, try again next round";
						update = false;
					}
					else
						d->t.d = t;
				}
			}

			// update data if something has changed
			if(update)
				Q_EMIT dataChanged();
			d->m.unlock();
		}
		sleep(5);
	}
}

void QSmartCard::selectCard(const QString &card)
{
	QMutexLocker locker(&d->m);
	QSharedDataPointer<QSmartCardDataPrivate> t = d->t.d;
	t->card = card;
	t->data.clear();
	t->appletVersion.clear();
	t->authCert = QSslCertificate();
	t->signCert = QSslCertificate();
	d->t.d = t;
	Q_EMIT dataChanged();
}

QSmartCard::ErrorType QSmartCard::unblock(QSmartCardData::PinType type, const QString &pin, const QString &puk)
{
	QMutexLocker locker(&d->m);
	QSharedPointer<QPCSCReader> reader(d->connect(d->t.reader()));
	if(!reader)
		return UnknownError;

	QByteArray cmd = d->VERIFY;
	QPCSCReader::Result result;

	if(!d->t.isPinpad())
	{
		//Verify PUK. Not for pinpad.
		cmd[3] = 0;
		cmd[4] = char(puk.size());
		result = reader->transfer(cmd + puk.toUtf8());
		if(!result)
			return d->handlePinResult(reader.data(), result, false);
	}

	// Make sure pin is locked. ID card is designed so that only blocked PIN could be unblocked with PUK!
	cmd[3] = type;
	cmd[4] = char(pin.size() + 1);
	for(quint8 i = 0; i <= d->t.retryCount(type); ++i)
		reader->transfer(cmd + QByteArray(pin.size(), '0') + QByteArray::number(i));

	//Replace PIN with PUK
	cmd = d->REPLACE;
	cmd[3] = type;
	cmd[4] = char(puk.size() + pin.size());
	if(d->t.isPinpad())
	{
		QEventLoop l;
		std::thread([&]{
			result = reader->transferCTL(cmd, false, d->language(), QSmartCardData::minPinLen(type));
			l.quit();
		}).detach();
		l.exec();
	}
	else
		result = reader->transfer(cmd + puk.toUtf8() + pin.toUtf8());
	return d->handlePinResult(reader.data(), result, true);
}
