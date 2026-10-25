#include "TraceLaunchDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace
{

// All machine targets the emulator's --machine flag understands. The tag
// is what we pass on the command line; the label is shown to the user.
struct MachineSpec
{
	const char* m_Label;
	const char* m_Tag;
};
constexpr MachineSpec kMachines[] = {
	{"Spectrum 48K", "48k"},
	{"Spectrum 128K", "128k"},
	{"Pentagon 128", "pentagon"},
	{"ZX Spectrum Next (50Hz)", "next50"},
	{"ZX Spectrum Next (60Hz)", "next60"},
	{"Timex Computer 2048", "tc2048"},
	{"Timex Computer 2068", "tc2068"},
	{"Timex Sinclair 2068", "ts2068"},
};
constexpr int kMachineCount = int(sizeof(kMachines) / sizeof(kMachines[0]));

bool MachineIsNext(int Idx)
{
	return Idx == 3 || Idx == 4; // next50 / next60
}

// Timex TC2048 / TC2068 / TS2068 - appended after the Next entries so the
// index-based checks stay valid. They're 48K-class (Spectrum memory layout,
// Home ROM loaded via --rom48), so they load 48K snapshots / tapes / .z80 but
// not 128K-paged snapshots or .nex.
bool MachineIsTimex(int Idx)
{
	return Idx >= 5 && Idx < kMachineCount;
}

// Where a file/dir picker should open so the user is never dumped into $HOME:
// the directory of whatever's already in the field (so re-picking a different
// file lands right where the current one is), else the last directory used for
// this kind of file (remembered across sessions in QSettings), else the folder
// the app was launched from (cwd). Never $HOME.
QString StartDirForPicker(const QString& CurrentText, const QString& SettingsKey)
{
	if (!CurrentText.isEmpty())
	{
		const QFileInfo Fi(CurrentText);
		const QString Dir = Fi.isDir() ? Fi.absoluteFilePath() : Fi.absolutePath();
		if (QFileInfo(Dir).isDir())
		{
			return Dir;
		}
	}
	const QString Remembered = QSettings().value(SettingsKey).toString();
	if (!Remembered.isEmpty() && QFileInfo(Remembered).isDir())
	{
		return Remembered;
	}
	return QDir::currentPath();
}

} // namespace

TraceLaunchDialog::TraceLaunchDialog(QWidget* Parent) : QDialog(Parent)
{
	setWindowTitle(tr("Trace a binary"));
	resize(720, 540);

	auto* Outer = new QVBoxLayout(this);

	auto* Form = new QFormLayout;
	Form->setRowWrapPolicy(QFormLayout::DontWrapRows);
	Form->setLabelAlignment(Qt::AlignRight);

	// Binary file picker.
	{
		auto* Row = new QHBoxLayout;
		m_Binary = new QLineEdit(this);
		m_Binary->setPlaceholderText(tr("Path to .sna / .z80 / .nex / .tap / .trd / .scl"));
		connect(m_Binary, &QLineEdit::editingFinished, this, &TraceLaunchDialog::OnBinaryEdited);
		m_BinBrowse = new QPushButton(tr("Browse..."), this);
		connect(m_BinBrowse, &QPushButton::clicked, this, &TraceLaunchDialog::OnBrowseBinary);
		Row->addWidget(m_Binary);
		Row->addWidget(m_BinBrowse);
		Form->addRow(tr("Binary:"), Row);
	}

	{
		auto* Row = new QHBoxLayout;
		m_Sld = new QLineEdit(this);
		m_Sld->setAccessibleName(tr("SLD symbols"));
		m_Sld->setPlaceholderText(tr("Optional matching .sld file"));
		m_SldBrowse = new QPushButton(tr("Browse..."), this);
		connect(m_SldBrowse, &QPushButton::clicked, this,
			[this]
			{
				const QString Path = QFileDialog::getOpenFileName(this, tr("Choose matching SLD symbols"),
					QFileInfo(m_Binary->text()).absolutePath(), tr("sjasmplus symbols (*.sld)"));
				if (!Path.isEmpty())
				{
					m_Sld->setText(Path);
				}
			});
		Row->addWidget(m_Sld, 1);
		Row->addWidget(m_SldBrowse);
		Form->addRow(tr("SLD symbols:"), Row);
	}

	// Machine type combo (with per-item enable/disable).
	{
		auto* Row = new QHBoxLayout;
		m_Machine = new QComboBox(this);
		auto* Model = new QStandardItemModel(this);
		for (int IdxI = 0; IdxI < kMachineCount; ++IdxI)
		{
			auto* Item = new QStandardItem(QString::fromUtf8(kMachines[IdxI].m_Label));
			Item->setData(QString::fromUtf8(kMachines[IdxI].m_Tag), Qt::UserRole + 1);
			Model->appendRow(Item);
		}
		m_Machine->setModel(Model);
		m_Machine->setCurrentIndex(3); // default next50; constrained by file
		// When the user switches machine, the TR-DOS auto-policy may change
		// (Pentagon forces it on, others leave it user-controlled).
		connect(m_Machine, qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this](int)
			{
				DetectAndConstrainMachine(m_Binary->text());
			});

		m_MachineNote = new QLabel(this);
		m_MachineNote->setStyleSheet(QStringLiteral("color: #888;"));

		Row->addWidget(m_Machine, 1);
		Row->addWidget(m_MachineNote);
		Form->addRow(tr("Machine:"), Row);
	}

	// RAM size - Next only.
	{
		auto* Row = new QHBoxLayout;
		m_RamMb = new QComboBox(this);
		m_RamMb->addItem(tr("1 MB"), 1);
		m_RamMb->addItem(tr("2 MB"), 2);
		m_RamMb->setCurrentIndex(1);
		m_RamNote = new QLabel(this);
		m_RamNote->setStyleSheet(QStringLiteral("color: #888;"));
		Row->addWidget(m_RamMb);
		Row->addWidget(m_RamNote);
		Row->addStretch();
		Form->addRow(tr("RAM (Next):"), Row);
	}

	// Run length: unit + value.
	{
		auto* Row = new QHBoxLayout;
		m_RunUnit = new QComboBox(this);
		m_RunUnit->addItem(tr("seconds"));
		m_RunUnit->addItem(tr("frames"));
		m_RunUnit->addItem(tr("cycles"));
		m_RunVal = new QSpinBox(this);
		m_RunVal->setRange(1, 1'000'000'000);
		m_RunVal->setValue(10);
		m_RunVal->setGroupSeparatorShown(true);
		Row->addWidget(m_RunUnit);
		Row->addWidget(m_RunVal, 1);
		Form->addRow(tr("Run for:"), Row);
	}

	m_TraceFromSeconds = new QSpinBox(this);
	m_TraceFromSeconds->setRange(0, 1000000);
	m_TraceFromSeconds->setSuffix(tr(" seconds"));
	m_TraceFromSeconds->setAccessibleName(tr("Start recording after"));
	m_TraceFromSeconds->setToolTip(
		tr("Emulate startup without recording it. Run for is the total emulation budget, including this delay."));
	Form->addRow(tr("Start recording after:"), m_TraceFromSeconds);

	// ROM dir - needed for SNA, optional for NEX.
	{
		auto* Row = new QHBoxLayout;
		m_RomDir = new QLineEdit(this);
		m_RomDir->setPlaceholderText(tr("Directory containing 48.rom / 128.rom"));
		m_RomDir->setText(FindRomDir());
		m_RomBrowse = new QPushButton(tr("Browse..."), this);
		connect(m_RomBrowse, &QPushButton::clicked, this, &TraceLaunchDialog::OnBrowseRomDir);
		m_RomNote = new QLabel(this);
		m_RomNote->setStyleSheet(QStringLiteral("color: #888;"));
		Row->addWidget(m_RomDir, 1);
		Row->addWidget(m_RomBrowse);
		Form->addRow(tr("ROM dir:"), Row);
		Form->addRow(QString(), m_RomNote);
	}

	// TR-DOS toggle. Many Pentagon-era demoscene SNAs rely on the
	// auto-page-in behaviour of fetches in $3D00-$3DFF. Pentagon BIOS BOOTS with TR-DOS
	// mapped, so we force-enable + lock the checkbox there. For 48K / 128K
	// / Next, default off - user toggles it when loading a TR-DOS demo.
	{
		auto* Row = new QHBoxLayout;
		m_TrdosCheck = new QCheckBox(tr("Enable TR-DOS ROM"), this);
		connect(m_TrdosCheck, &QCheckBox::toggled, this,
			[this](bool)
			{
				// Re-detect the binary kind so the ROM dir requirement
				// text refreshes (TR-DOS needs the trdos504t.rom).
				DetectAndConstrainMachine(m_Binary->text());
			});
		m_TrdosNote = new QLabel(this);
		m_TrdosNote->setStyleSheet(QStringLiteral("color: #888;"));
		Row->addWidget(m_TrdosCheck);
		Row->addWidget(m_TrdosNote);
		Row->addStretch();
		Form->addRow(tr("TR-DOS:"), Row);
	}

	// TR-DOS run-file selector - only visible for .trd/.scl images. Lists the
	// image's BASIC (".B") files; the picked one becomes --trd-run <name>.
	{
		auto* Row = new QHBoxLayout;
		m_TrdFile = new QComboBox(this);
		m_TrdFile->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		m_TrdFileNote = new QLabel(this);
		m_TrdFileNote->setStyleSheet(QStringLiteral("color: #888;"));
		Row->addWidget(m_TrdFile);
		Row->addWidget(m_TrdFileNote);
		Row->addStretch();
		m_TrdFileLabel = new QLabel(tr("Run file:"), this);
		Form->addRow(m_TrdFileLabel, Row);
		m_TrdFileLabel->setVisible(false);
		m_TrdFile->setVisible(false);
		m_TrdFileNote->setVisible(false);
	}

	// Output path - defaults to temp dir filename derived from binary.
	{
		auto* Row = new QHBoxLayout;
		m_Output = new QLineEdit(this);
		m_OutBrowse = new QPushButton(tr("Browse..."), this);
		connect(m_OutBrowse, &QPushButton::clicked, this, &TraceLaunchDialog::OnBrowseOutput);
		Row->addWidget(m_Output, 1);
		Row->addWidget(m_OutBrowse);
		Form->addRow(tr("Output (.btr):"), Row);
	}

	Outer->addLayout(Form);

	// Process log.
	m_Log = new QPlainTextEdit(this);
	m_Log->setReadOnly(true);
	QFont Mono = m_Log->font();
	Mono.setFamily(QStringLiteral("monospace"));
	Mono.setStyleHint(QFont::Monospace);
	m_Log->setFont(Mono);
	m_Log->setPlaceholderText(tr("Emulator output appears here once tracing starts..."));
	Outer->addWidget(m_Log, 1);

	// Buttons.
	{
		auto* Row = new QHBoxLayout;
		Row->addStretch();
		m_BtnCancel = new QPushButton(tr("Cancel"), this);
		m_BtnTrace = new QPushButton(tr("Trace"), this);
		m_BtnTraceOpen = new QPushButton(tr("Trace and Open"), this);
		m_BtnTraceOpen->setDefault(true);
		connect(m_BtnCancel, &QPushButton::clicked, this, &QDialog::reject);
		connect(m_BtnTrace, &QPushButton::clicked, this, &TraceLaunchDialog::OnTrace);
		connect(m_BtnTraceOpen, &QPushButton::clicked, this, &TraceLaunchDialog::OnTraceAndOpen);
		Row->addWidget(m_BtnCancel);
		Row->addWidget(m_BtnTrace);
		Row->addWidget(m_BtnTraceOpen);
		Outer->addLayout(Row);
	}

	// Set initial constraints with empty binary path - disables nothing,
	// shows neutral note.
	DetectAndConstrainMachine(QString());
}

TraceLaunchDialog::~TraceLaunchDialog() = default;

void TraceLaunchDialog::PresetBinary(const QString& Path)
{
	m_Binary->setText(Path);
	OnBinaryEdited();
}

void TraceLaunchDialog::PresetMachine(int Idx)
{
	if (Idx >= 0 && Idx < kMachineCount)
	{
		m_Machine->setCurrentIndex(Idx);
	}
}

QVariantMap TraceLaunchDialog::CurrentParams() const
{
	QVariantMap P;
	P[QStringLiteral("binary")] = m_Binary->text().trimmed();
	P[QStringLiteral("machine")] = m_Machine->itemData(m_Machine->currentIndex(), Qt::UserRole + 1).toString();
	P[QStringLiteral("ramMb")] = m_RamMb->currentData().toInt();
	P[QStringLiteral("runUnit")] = m_RunUnit->currentIndex();
	P[QStringLiteral("runVal")] = m_RunVal->value();
	P[QStringLiteral("traceFromSeconds")] = m_TraceFromSeconds->value();
	P[QStringLiteral("sld")] = m_Sld->text().trimmed();
	P[QStringLiteral("romDir")] = m_RomDir->text().trimmed();
	P[QStringLiteral("trdos")] = m_TrdosCheck->isChecked();
	P[QStringLiteral("trdFile")] = m_TrdFile->currentText();
	P[QStringLiteral("output")] = m_Output->text().trimmed();
	return P;
}

void TraceLaunchDialog::ApplyParams(const QVariantMap& P)
{
	// Binary first: presetBinary runs detectAndConstrainMachine + the default-
	// output / run-length cascade, which we then override with the saved values.
	const QString Binary = P.value(QStringLiteral("binary")).toString();
	if (!Binary.isEmpty())
	{
		PresetBinary(Binary);
	}
	// Machine: pick the combo row whose --machine tag matches the saved one.
	const QString Tag = P.value(QStringLiteral("machine")).toString();
	for (int IdxI = 0; IdxI < m_Machine->count(); ++IdxI)
	{
		if (m_Machine->itemData(IdxI, Qt::UserRole + 1).toString() == Tag)
		{
			m_Machine->setCurrentIndex(IdxI);
			break;
		}
	}
	if (P.contains(QStringLiteral("ramMb")))
	{
		const int Idx = m_RamMb->findData(P.value(QStringLiteral("ramMb")).toInt());
		if (Idx >= 0)
		{
			m_RamMb->setCurrentIndex(Idx);
		}
	}
	if (P.contains(QStringLiteral("runUnit")))
	{
		m_RunUnit->setCurrentIndex(P.value(QStringLiteral("runUnit")).toInt());
	}
	if (P.contains(QStringLiteral("runVal")))
	{
		m_RunVal->setValue(P.value(QStringLiteral("runVal")).toInt());
	}
	const QString RomDir = P.value(QStringLiteral("romDir")).toString();
	m_TraceFromSeconds->setValue(P.value(QStringLiteral("traceFromSeconds"), 0).toInt());
	if (P.contains(QStringLiteral("sld")))
	{
		m_Sld->setText(P.value(QStringLiteral("sld")).toString());
	}
	if (!RomDir.isEmpty())
	{
		m_RomDir->setText(RomDir);
	}
	if (P.contains(QStringLiteral("trdos")))
	{
		m_TrdosCheck->setChecked(P.value(QStringLiteral("trdos")).toBool());
	}
	// TR-DOS run-file: only matches if the detect above populated the list.
	const QString TrdFile = P.value(QStringLiteral("trdFile")).toString();
	if (!TrdFile.isEmpty())
	{
		const int Idx = m_TrdFile->findText(TrdFile);
		if (Idx >= 0)
		{
			m_TrdFile->setCurrentIndex(Idx);
		}
	}
	const QString Output = P.value(QStringLiteral("output")).toString();
	if (!Output.isEmpty())
	{
		m_Output->setText(Output);
	}
}

QList<QVariantMap> TraceLaunchDialog::LoadRecentParams()
{
	QSettings S;
	QList<QVariantMap> OutValue;
	const QVariantList Raw = S.value(QStringLiteral("recentTraceParams")).toList();
	for (const QVariant& V : Raw)
	{
		const QVariantMap M = V.toMap();
		if (!M.value(QStringLiteral("binary")).toString().isEmpty())
		{
			OutValue.append(M);
		}
	}
	return OutValue;
}

void TraceLaunchDialog::RememberParams(const QVariantMap& P)
{
	if (P.value(QStringLiteral("binary")).toString().isEmpty())
	{
		return;
	}
	QList<QVariantMap> List = LoadRecentParams();
	// Drop an exact duplicate so re-running the same trace bumps it to the top.
	for (int IdxI = List.size() - 1; IdxI >= 0; --IdxI)
	{
		if (List[IdxI] == P)
		{
			List.removeAt(IdxI);
		}
	}
	List.prepend(P);
	while (List.size() > 10)
	{
		List.removeLast();
	}
	QVariantList Raw;
	for (const QVariantMap& M : List)
	{
		Raw.append(M);
	}
	QSettings S;
	S.setValue(QStringLiteral("recentTraceParams"), Raw);
}

QString TraceLaunchDialog::ParamsLabel(const QVariantMap& P)
{
	const QString Bin = QFileInfo(P.value(QStringLiteral("binary")).toString()).fileName();
	const QString Mach = P.value(QStringLiteral("machine")).toString();
	const int Unit = P.value(QStringLiteral("runUnit")).toInt();
	const int Val = P.value(QStringLiteral("runVal")).toInt();
	static const char* const kUnits[] = {"s", "frames", "cyc"};
	const QString U = (Unit >= 0 && Unit < 3) ? QString::fromLatin1(kUnits[Unit]) : QString();
	return QStringLiteral("%1  [%2, %3 %4]").arg(Bin, Mach).arg(Val).arg(U);
}

void TraceLaunchDialog::OnBrowseBinary()
{
	const QString Start = StartDirForPicker(m_Binary->text(), QStringLiteral("lastTraceBinaryDir"));
	const QString Path = QFileDialog::getOpenFileName(this, tr("Choose binary"), Start,
		tr("ZX Spectrum binaries (*.sna *.z80 *.nex *.tap *.trd *.scl);;All files (*)"));
	if (Path.isEmpty())
	{
		return;
	}
	QSettings().setValue(QStringLiteral("lastTraceBinaryDir"), QFileInfo(Path).absolutePath());
	m_Binary->setText(Path);
	OnBinaryEdited();
}

void TraceLaunchDialog::OnBrowseRomDir()
{
	const QString Start = StartDirForPicker(m_RomDir->text(), QStringLiteral("lastRomDir"));
	const QString D = QFileDialog::getExistingDirectory(this, tr("ROM directory"), Start);
	if (D.isEmpty())
	{
		return;
	}
	QSettings().setValue(QStringLiteral("lastRomDir"), D);
	m_RomDir->setText(D);
}

void TraceLaunchDialog::OnBrowseOutput()
{
	const QString Start = StartDirForPicker(m_Output->text(), QStringLiteral("lastOutputDir"));
	const QString P = QFileDialog::getSaveFileName(this, tr("Output .btr"), Start, tr("Binary trace files (*.btr)"));
	if (P.isEmpty())
	{
		return;
	}
	QSettings().setValue(QStringLiteral("lastOutputDir"), QFileInfo(P).absolutePath());
	m_Output->setText(P);
}

void TraceLaunchDialog::OnBinaryEdited()
{
	const QString P = m_Binary->text();
	const QFileInfo BinaryInfo(P);
	const QString AdjacentSld =
		BinaryInfo.absolutePath() + QChar('/') + BinaryInfo.completeBaseName() + QStringLiteral(".sld");
	// Update only an automatically chosen path; preserve a user's explicit choice.
	if (m_Sld->text().isEmpty() || m_Sld->text() == m_Sld->property("autoPath").toString())
	{
		m_Sld->setText(QFile::exists(AdjacentSld) ? AdjacentSld : QString());
		m_Sld->setProperty("autoPath", m_Sld->text());
	}
	DetectAndConstrainMachine(P);
	if (m_Output->text().isEmpty() && !P.isEmpty())
	{
		m_Output->setText(DefaultOutputFor(P));
	}
	// Tape / disk sources cold-boot the ROM and auto-type before the program
	// even starts (~900 frames for a TR-DOS load), so the pristine "10 seconds"
	// default would trace only the boot. Bump it to a load-covering frame count
	// - but only from the untouched default, so a user's own choice is kept.
	const QString Kind = DetectBinaryKind(P);
	const bool bBootLoad = (Kind == QStringLiteral("tap") || Kind == QStringLiteral("trd"));
	if (bBootLoad && m_RunUnit->currentIndex() == 0 && m_RunVal->value() == 10)
	{
		m_RunUnit->setCurrentIndex(1); // frames
		m_RunVal->setValue(1500);
	}
}

QString TraceLaunchDialog::DetectBinaryKind(const QString& Path)
{
	QFileInfo Fi(Path);
	if (!Fi.exists() || !Fi.isReadable() || !Fi.isFile())
	{
		return QStringLiteral("unknown");
	}
	// Extension-driven kinds first (tape / disk / z80 have no unique magic that
	// beats a clear suffix). A .scl is also recognised by its SINCLAIR signature.
	if (Path.endsWith(QStringLiteral(".tap"), Qt::CaseInsensitive))
	{
		return QStringLiteral("tap");
	}
	if (Path.endsWith(QStringLiteral(".trd"), Qt::CaseInsensitive) ||
		Path.endsWith(QStringLiteral(".scl"), Qt::CaseInsensitive))
	{
		return QStringLiteral("trd");
	}
	if (Path.endsWith(QStringLiteral(".z80"), Qt::CaseInsensitive))
	{
		return QStringLiteral("z80");
	}
	QFile F(Path);
	if (!F.open(QIODevice::ReadOnly))
	{
		return QStringLiteral("unknown");
	}
	const QByteArray Head = F.read(8);
	// NEX: first four bytes spell "Next" - second word is "V1.x".
	if (Head.size() >= 4 && Head.startsWith("Next"))
	{
		return QStringLiteral("nex");
	}
	// SCL disk image without a .scl suffix - sniff its 8-byte SINCLAIR signature.
	if (Head == QByteArrayLiteral("SINCLAIR"))
	{
		return QStringLiteral("trd");
	}
	// SNA size sniff: 48K SNA is exactly 49179 bytes; everything larger
	// (131103 / 147487 / etc.) is a paged 128K snapshot.
	const qint64 Sz = Fi.size();
	if (Sz == 49179)
	{
		return QStringLiteral("sna48");
	}
	if (Sz == 131103 || Sz == 147487)
	{
		return QStringLiteral("sna128");
	}
	if (Path.endsWith(QStringLiteral(".sna"), Qt::CaseInsensitive))
	{
		return Sz <= 49179 ? QStringLiteral("sna48") : QStringLiteral("sna128");
	}
	return QStringLiteral("unknown");
}

QStringList TraceLaunchDialog::ListTrdBasicFiles(const QString& Path)
{
	QStringList OutValue;
	QFile F(Path);
	if (!F.open(QIODevice::ReadOnly))
	{
		return OutValue;
	}

	auto Rtrim = [](QByteArray B) -> QString
	{
		int N = B.size();
		while (N > 0 && (B[N - 1] == ' ' || B[N - 1] == '\0'))
		{
			--N;
		}
		return QString::fromLatin1(B.constData(), N);
	};

	const QByteArray Head = F.read(8);
	if (Head == QByteArrayLiteral("SINCLAIR"))
	{
		// .scl: "SINCLAIR" + count(1) + count x 14-byte descriptors
		//       (name[8], ext, start(2), len(2), sectors).
		const QByteArray Cnt = F.read(1);
		if (Cnt.isEmpty())
		{
			return OutValue;
		}
		const int N = quint8(Cnt[0]);
		const QByteArray Desc = F.read(qint64(N) * 14);
		for (int IdxI = 0; IdxI < N && (IdxI * 14 + 14) <= Desc.size(); ++IdxI)
		{
			const int O = IdxI * 14;
			if (Desc[O + 8] != 'B')
			{
				continue; // ext byte
			}
			OutValue << Rtrim(Desc.mid(O, 8));
		}
		return OutValue;
	}

	// .trd: catalogue = track 0, sectors 0-7 = first 2048 bytes; 128 x 16-byte
	// entries (name[8], ext, start(2), len(2), sectors, startsec, starttrk).
	F.seek(0);
	const QByteArray Cat = F.read(2048);
	for (int E = 0; E < 128 && (E * 16 + 16) <= Cat.size(); ++E)
	{
		const int O = E * 16;
		const quint8 kC0 = quint8(Cat[O]);
		if (kC0 == 0x00)
		{
			break; // end of catalogue
		}
		if (kC0 == 0x01)
		{
			continue; // deleted entry
		}
		if (Cat[O + 8] != 'B')
		{
			continue;
		}
		OutValue << Rtrim(Cat.mid(O, 8));
	}
	return OutValue;
}

void TraceLaunchDialog::DetectAndConstrainMachine(const QString& BinaryPath)
{
	auto* Model = qobject_cast<QStandardItemModel*>(m_Machine->model());
	if (Model == nullptr)
	{
		return;
	}

	// Reset all to enabled, then constrain based on file kind.
	for (int IdxI = 0; IdxI < kMachineCount; ++IdxI)
	{
		Model->item(IdxI)->setEnabled(true);
	}

	const QString Kind = DetectBinaryKind(BinaryPath);
	QString Note;
	int DesiredIndex = m_Machine->currentIndex();
	bool bNextOnly = false;

	if (Kind == QStringLiteral("nex"))
	{
		// .nex must run on Next. Disable everything else.
		for (int IdxI = 0; IdxI < kMachineCount; ++IdxI)
		{
			if (!MachineIsNext(IdxI))
			{
				Model->item(IdxI)->setEnabled(false);
			}
		}
		if (!MachineIsNext(DesiredIndex))
		{
			DesiredIndex = 3;
		}
		Note = tr(".nex requires Next 50/60 - other targets disabled");
		bNextOnly = true;
	}
	else if (Kind == QStringLiteral("sna128"))
	{
		// 128K SNA carries paged banks - 48K and the 48K-class Timex machines
		// (no 128K paging) can't load it.
		Model->item(0)->setEnabled(false);
		for (int IdxI = 0; IdxI < kMachineCount; ++IdxI)
		{
			if (MachineIsTimex(IdxI))
			{
				Model->item(IdxI)->setEnabled(false);
			}
		}
		if (DesiredIndex == 0 || MachineIsTimex(DesiredIndex))
		{
			DesiredIndex = 1;
		}
		Note = tr("128K snapshot - Spectrum 48K and Timex disabled");
	}
	else if (Kind == QStringLiteral("sna48"))
	{
		// 48K SNA fits everything.
		Note = tr("48K snapshot - all targets supported");
	}
	else if (Kind == QStringLiteral("tap") || Kind == QStringLiteral("trd") || Kind == QStringLiteral("z80"))
	{
		// Classic tape / disk / .z80 - not a Next format. Disable Next targets.
		for (int IdxI = 0; IdxI < kMachineCount; ++IdxI)
		{
			if (MachineIsNext(IdxI))
			{
				Model->item(IdxI)->setEnabled(false);
			}
		}
		if (MachineIsNext(DesiredIndex))
		{
			DesiredIndex = 1; // default 128K
		}
		Note = (Kind == QStringLiteral("tap"))	 ? tr("Tape image - cold-boots the ROM and auto-types LOAD \"\"")
			   : (Kind == QStringLiteral("trd")) ? tr("TR-DOS disk - pick a BASIC file below to run by name")
												 : tr(".z80 snapshot - machine auto-detected by the emulator");
	}
	else if (BinaryPath.isEmpty())
	{
		Note = tr("Pick a binary to constrain the machine list.");
	}
	else
	{
		Note = tr("Unrecognised file. Manual selection in effect.");
	}
	m_MachineNote->setText(Note);
	m_Machine->setCurrentIndex(DesiredIndex);

	// RAM (Next-only). For .nex, note that 2MB isn't required by the file.
	const bool bNext = MachineIsNext(m_Machine->currentIndex()) || bNextOnly;
	m_RamMb->setEnabled(bNext);
	if (!bNext)
	{
		m_RamNote->setText(tr("(Next only)"));
	}
	else if (Kind == QStringLiteral("nex"))
	{
		m_RamNote->setText(tr(".nex does not require 2MB"));
	}
	else
	{
		m_RamNote->setText(QString());
	}

	// ROM dir requirement: .sna / .z80 / .tap / .trd need ROMs; .nex / unknown /
	// empty don't. TR-DOS disks additionally need trdos504t.rom.
	const bool bTrd = (Kind == QStringLiteral("trd"));
	const bool bRomNeeded = Kind.startsWith(QStringLiteral("sna")) || Kind == QStringLiteral("tap") ||
							Kind == QStringLiteral("z80") || bTrd;
	m_RomDir->setEnabled(bRomNeeded);
	m_RomBrowse->setEnabled(bRomNeeded);
	if (bRomNeeded)
	{
		const QString Dir = m_RomDir->text();
		const bool bBase = !Dir.isEmpty() && QFile::exists(Dir + QStringLiteral("/48.rom")) &&
						   QFile::exists(Dir + QStringLiteral("/128.rom"));
		const bool bTrdRom = QFile::exists(Dir + QStringLiteral("/trdos504t.rom"));
		if (!bBase)
		{
			m_RomNote->setText(tr("48.rom and 128.rom must be in this directory."));
		}
		else if (bTrd && !bTrdRom)
		{
			m_RomNote->setText(tr("TR-DOS disk also needs trdos504t.rom in this directory."));
		}
		else
		{
			m_RomNote->setText(tr("ROMs found."));
		}
	}
	else
	{
		m_RomNote->setText(tr("(not needed for this binary)"));
	}

	// TR-DOS run-file selector: visible only for .trd/.scl. Repopulate the
	// BASIC-file list when the chosen image changes.
	RefreshTrdFileList(bTrd ? BinaryPath : QString());

	// TR-DOS auto-policy:
	//   Pentagon -> force on, lock the checkbox (Pentagon BIOS boots
	//              with TR-DOS mapped - running without it would deviate
	//              from real-hardware behaviour).
	//   .nex     -> force off, lock (Next .nex files don't use TR-DOS).
	//   else     -> user choice, default off (but stays user's last toggle
	//              if they edited binary path without flipping machine).
	const int Midx = m_Machine->currentIndex();
	const bool bPentagon = (Midx == 2);
	const bool bNexBinary = (Kind == QStringLiteral("nex"));
	if (bTrd)
	{
		// A .trd/.scl is driven through TR-DOS by definition; the --trd-run flow
		// mounts + auto-activates it, so force the toggle on and lock it.
		m_TrdosCheck->setChecked(true);
		m_TrdosCheck->setEnabled(false);
		m_TrdosNote->setText(tr("(required for TR-DOS disks)"));
	}
	else if (bPentagon)
	{
		m_TrdosCheck->setChecked(true);
		m_TrdosCheck->setEnabled(false);
		m_TrdosNote->setText(tr("(forced on for Pentagon)"));
	}
	else if (bNexBinary)
	{
		m_TrdosCheck->setChecked(false);
		m_TrdosCheck->setEnabled(false);
		m_TrdosNote->setText(tr("(not used for .nex)"));
	}
	else
	{
		m_TrdosCheck->setEnabled(bRomNeeded);
		m_TrdosNote->setText(
			bRomNeeded ? tr("enable for TR-DOS demos (Pentagon-era)") : tr("(not applicable - .nex / unknown binary)"));
	}
}

void TraceLaunchDialog::RefreshTrdFileList(const QString& Path)
{
	const bool bShow = !Path.isEmpty();
	if (m_TrdFileLabel)
	{
		m_TrdFileLabel->setVisible(bShow);
	}
	m_TrdFile->setVisible(bShow);
	m_TrdFileNote->setVisible(bShow);
	if (!bShow)
	{
		m_TrdFile->clear();
		return;
	}

	// Preserve the current selection across a re-detect of the same image.
	const QString Prev = m_TrdFile->currentText();
	const QStringList Files = ListTrdBasicFiles(Path);
	m_TrdFile->clear();
	m_TrdFile->addItems(Files);
	if (Files.isEmpty())
	{
		m_TrdFile->setEnabled(false);
		m_TrdFileNote->setText(tr("no BASIC (.B) files found in this image"));
	}
	else
	{
		m_TrdFile->setEnabled(true);
		const int Keep = m_TrdFile->findText(Prev);
		m_TrdFile->setCurrentIndex(Keep >= 0 ? Keep : 0);
		m_TrdFileNote->setText(tr("%n BASIC file(s)", nullptr, int(Files.size())));
	}
}

QString TraceLaunchDialog::DefaultOutputFor(const QString& BinaryPath) const
{
	QFileInfo Fi(BinaryPath);
	const QString Base = Fi.completeBaseName();
	const QString Stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
	const QString Dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	return QDir(Dir).filePath(QStringLiteral("%1-%2.btr").arg(Base, Stamp));
}

QString TraceLaunchDialog::FindEmulatorBinary() const
{
	// Executable name (Windows needs the .exe suffix for QFile::exists to match).
#ifdef Q_OS_WIN
	const QString Exe = QStringLiteral("inspeccy.exe");
#else
	const QString Exe = QStringLiteral("inspeccy");
#endif
	const QDir AppDir(QCoreApplication::applicationDirPath());

	// Probe known layouts, nearest first. The CLI must be the fresh, co-built
	// one - otherwise a stale inspeccy on PATH silently omits the .fidx frame
	// index and the timeline stays blank. `cfg` is this build's config subdir
	// (Release/Debug) so the MSVC build tree, which puts each target under its
	// own <target>/<cfg>/, is matched too.
	const QString Cfg = AppDir.dirName();
	const QString Rel[] = {
		Exe, // installed bin/: side by side
		QStringLiteral("../inspeccy/") + Exe, // flat build/install
		QStringLiteral("../../inspeccy/") + Cfg + "/" + Exe, // MSVC: build/inspeccy/<cfg>/
		QStringLiteral("../inspeccy/") + Cfg + "/" + Exe,
	};
	for (const QString& R : Rel)
	{
		const QString Cand = AppDir.absoluteFilePath(R);
		if (QFile::exists(Cand))
		{
			return QDir::cleanPath(Cand);
		}
	}

	// PATH fallback.
	const QString PathEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
	for (const QString& Dir : PathEnv.split(QDir::listSeparator(), Qt::SkipEmptyParts))
	{
		const QString Cand = QDir(Dir).absoluteFilePath(Exe);
		if (QFile::exists(Cand))
		{
			return Cand;
		}
	}
	return QStringLiteral("inspeccy"); // let QProcess error out clearly
}

QString TraceLaunchDialog::FindRomDir() const
{
	// Share the test runner's explicit ROM directory; retain the older viewer
	// variable for existing installations. Saved dialog paths take precedence.
	const auto Environment = QProcessEnvironment::systemEnvironment();
	for (const QString& Name : {QStringLiteral("INSPECCY_ROM_DIR"), QStringLiteral("INSPECCY_ROMS")})
	{
		const QString Env = Environment.value(Name);
		if (!Env.isEmpty() && QFile::exists(Env + QStringLiteral("/48.rom")))
		{
			return Env;
		}
	}

	const QDir AppDir(QCoreApplication::applicationDirPath());
	const QStringList Candidates = {
		AppDir.absoluteFilePath(QStringLiteral("../../../roms")),
		AppDir.absoluteFilePath(QStringLiteral("../../roms")),
		AppDir.absoluteFilePath(QStringLiteral("../roms")),
		QDir(QStringLiteral("roms")).absolutePath(),
	};
	for (const QString& C : Candidates)
	{
		if (QFile::exists(C + QStringLiteral("/48.rom")))
		{
			return QDir::cleanPath(C);
		}
	}
	return {};
}

void TraceLaunchDialog::OnTrace()
{
	RunTrace(false);
}
void TraceLaunchDialog::OnTraceAndOpen()
{
	RunTrace(true);
}

void TraceLaunchDialog::RunTrace(bool bOpenAfter)
{
	m_bOpenAfter = bOpenAfter;
	const QString BinPath = m_Binary->text().trimmed();
	if (BinPath.isEmpty() || !QFile::exists(BinPath))
	{
		m_Log->appendPlainText(tr("Pick a binary first."));
		return;
	}
	const QString OutPath = m_Output->text().trimmed();
	if (OutPath.isEmpty())
	{
		m_Log->appendPlainText(tr("Set an output path first."));
		return;
	}
	const QString Kind = DetectBinaryKind(BinPath);
	const bool bTrd = (Kind == QStringLiteral("trd"));
	const bool bRomNeeded = Kind.startsWith(QStringLiteral("sna")) || Kind == QStringLiteral("tap") ||
							Kind == QStringLiteral("z80") || bTrd;
	const QString Dir = m_RomDir->text().trimmed();
	if (bRomNeeded)
	{
		if (Dir.isEmpty() || !QFile::exists(Dir + QStringLiteral("/48.rom")) ||
			!QFile::exists(Dir + QStringLiteral("/128.rom")))
		{
			m_Log->appendPlainText(tr("ROM dir needs both 48.rom and 128.rom for this target."));
			return;
		}
		if (bTrd && !QFile::exists(Dir + QStringLiteral("/trdos504t.rom")))
		{
			m_Log->appendPlainText(tr("TR-DOS disk needs trdos504t.rom in the ROM directory."));
			return;
		}
	}
	if (bTrd && m_TrdFile->currentText().isEmpty())
	{
		m_Log->appendPlainText(tr("Pick a BASIC file to run from the disk."));
		return;
	}

	QStringList Args;
	const QString SldPath = m_Sld->text().trimmed();
	if (!SldPath.isEmpty())
	{
		if (!QFileInfo(SldPath).isFile())
		{
			m_Log->appendPlainText(tr("SLD file does not exist."));
			return;
		}
		Args << QStringLiteral("--sld") << SldPath;
	}
	if (m_TraceFromSeconds->value() > 0)
	{
		if (m_RunUnit->currentIndex() == 0 && m_TraceFromSeconds->value() >= m_RunVal->value())
		{
			m_Log->appendPlainText(tr("Start recording must be earlier than the end of the run."));
			return;
		}
		Args << QStringLiteral("--trace-from-seconds") << QString::number(m_TraceFromSeconds->value());
	}
	Args << QStringLiteral("--machine") << m_Machine->itemData(m_Machine->currentIndex(), Qt::UserRole + 1).toString();
	// Source flag per kind.
	if (Kind == QStringLiteral("nex"))
	{
		Args << QStringLiteral("--nex") << BinPath;
	}
	else if (Kind == QStringLiteral("tap"))
	{
		Args << QStringLiteral("--tap") << BinPath;
	}
	else if (Kind == QStringLiteral("z80"))
	{
		Args << QStringLiteral("--z80") << BinPath;
	}
	else if (bTrd)
	{
		Args << QStringLiteral("--trd") << BinPath << QStringLiteral("--trd-run") << m_TrdFile->currentText();
	}
	else
	{
		Args << QStringLiteral("--sna") << BinPath;
	}
	// ROMs (all classic targets; harmless to always pass the TR-DOS ROM when present).
	if (bRomNeeded)
	{
		Args << QStringLiteral("--rom48") << Dir + QStringLiteral("/48.rom");
		Args << QStringLiteral("--rom128") << Dir + QStringLiteral("/128.rom");
		const QString Trdos = Dir + QStringLiteral("/trdos504t.rom");
		if (QFile::exists(Trdos))
		{
			Args << QStringLiteral("--rom-trdos") << Trdos;
		}
		// TR-DOS auto-page-in: --trd-run enables it implicitly; for SNA/z80/tap
		// the checkbox gives explicit control (Pentagon -> forced on).
		if (!bTrd && m_TrdosCheck->isChecked())
		{
			Args << QStringLiteral("--trdos") << QStringLiteral("on");
		}
	}
	if (MachineIsNext(m_Machine->currentIndex()))
	{
		Args << QStringLiteral("--ram-mb") << QString::number(m_RamMb->currentData().toInt());
	}
	const int N = m_RunVal->value();
	switch (m_RunUnit->currentIndex())
	{
	case 0:
		Args << QStringLiteral("--seconds") << QString::number(N);
		break;
	case 1:
		Args << QStringLiteral("--until-frame") << QString::number(N);
		break;
	case 2:
		Args << QStringLiteral("--cycles") << QString::number(N);
		break;
	default:
		break;
	}
	// Emit the .fidx frame index next to the .btr (the viewer reads it as
	// <btr>.fidx). The CLI writes it by default, but we pass the path explicitly
	// so the timeline overview is guaranteed regardless of CLI defaults.
	Args << QStringLiteral("--trace-out-bin") << OutPath << QStringLiteral("--trace-index-out")
		 << (OutPath + QStringLiteral(".fidx")) << QStringLiteral("--no-bmp") << QStringLiteral("--no-status")
		 << QStringLiteral("--no-dump");

	const QString Emu = FindEmulatorBinary();
	m_OutputPath = OutPath;
	// Remember this parameter set for the History menu (most-recent-first, cap 10).
	RememberParams(CurrentParams());
	m_Log->clear();
	m_Log->appendPlainText(QStringLiteral("$ %1 %2").arg(Emu, Args.join(QChar(' '))));
	SetRunning(true);

	m_Proc = new QProcess(this);
	m_Proc->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_Proc, &QProcess::readyReadStandardOutput, this, &TraceLaunchDialog::OnProcessReadyRead);
	connect(m_Proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
		[this](int Code, QProcess::ExitStatus St)
		{
			OnProcessFinished(Code, int(St));
		});
	m_Proc->start(Emu, Args);
	if (!m_Proc->waitForStarted(3000))
	{
		m_Log->appendPlainText(tr("Failed to start: %1").arg(m_Proc->errorString()));
		SetRunning(false);
		m_Proc->deleteLater();
		m_Proc = nullptr;
		return;
	}
}

void TraceLaunchDialog::SetRunning(bool bOn)
{
	m_BinBrowse->setEnabled(!bOn);
	m_Binary->setEnabled(!bOn);
	m_Machine->setEnabled(!bOn);
	m_RamMb->setEnabled(!bOn);
	m_RunUnit->setEnabled(!bOn);
	m_RunVal->setEnabled(!bOn);
	m_TraceFromSeconds->setEnabled(!bOn);
	m_Sld->setEnabled(!bOn);
	m_SldBrowse->setEnabled(!bOn);
	m_RomDir->setEnabled(!bOn);
	m_RomBrowse->setEnabled(!bOn);
	if (m_TrdFile)
	{
		m_TrdFile->setEnabled(!bOn && m_TrdFile->count() > 0);
	}
	m_Output->setEnabled(!bOn);
	m_OutBrowse->setEnabled(!bOn);
	m_BtnTrace->setEnabled(!bOn);
	m_BtnTraceOpen->setEnabled(!bOn);
	// m_BtnCancel keeps cancelling whether running or not (rejects -> kills
	// process via dtor cleanup path below).
}

void TraceLaunchDialog::OnProcessReadyRead()
{
	if (m_Proc == nullptr)
	{
		return;
	}
	const QByteArray Data = m_Proc->readAllStandardOutput();
	m_Log->appendPlainText(QString::fromUtf8(Data).trimmed());
}

void TraceLaunchDialog::OnProcessFinished(int ExitCode, int ExitStatus)
{
	SetRunning(false);
	if (m_Proc != nullptr)
	{
		const QByteArray Tail = m_Proc->readAllStandardOutput();
		if (!Tail.isEmpty())
		{
			m_Log->appendPlainText(QString::fromUtf8(Tail).trimmed());
		}
		m_Proc->deleteLater();
		m_Proc = nullptr;
	}
	if (ExitStatus != int(QProcess::NormalExit) || ExitCode != 0)
	{
		m_Log->appendPlainText(tr("Emulator exited with code %1 - trace likely incomplete.").arg(ExitCode));
		return;
	}
	if (!QFile::exists(m_OutputPath))
	{
		m_Log->appendPlainText(tr("Emulator finished but %1 wasn't created.").arg(m_OutputPath));
		return;
	}
	m_Outcome = m_bOpenAfter ? kOutcomeTraceAndOpen : kOutcomeTraced;
	accept();
}
