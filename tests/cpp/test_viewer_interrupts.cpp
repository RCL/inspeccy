#include "TraceModel.h"
#include "InterruptView.h"
#include "IcicleWidget.h"
#include "TraceLaunchDialog.h"
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QLabel>
#include <cstdlib>
#include <cstdio>
#include <cmath>

using namespace BinaryTrace;
static void Check(bool Value, const char* Message)
{
	if (!Value)
	{
		fprintf(stderr, "FAIL: %s\n", Message);
		std::exit(1);
	}
}
template <typename T> static void Put(QByteArray& Bytes, T Value)
{
	Bytes.append(reinterpret_cast<const char*>(&Value), sizeof(Value));
}
static void Var(QByteArray& Bytes, uint64_t Value)
{
	do
	{
		const auto Byte = uint8_t((Value & 127) | (Value > 127 ? 128 : 0));
		Put(Bytes, Byte);
		Value >>= 7;
	} while (Value);
}
static void Save(const QString& Path, const QByteArray& Bytes)
{
	QFile File(Path);
	Check(File.open(QIODevice::WriteOnly), "open fixture");
	Check(File.write(Bytes) == Bytes.size(), "write fixture");
}
int main(int Argc, char** Argv)
{
	QApplication App(Argc, Argv);
	QTemporaryDir Dir;
	Check(Dir.isValid(), "temporary fixture directory");
	Header H{};
	H.m_Magic = kMagic;
	H.m_Version = kVersion;
	H.m_EndCycle = 4000;
	H.m_EndTimeNanos = 1000000;
	H.m_BaseMhzX1e6 = 3500000;
	H.m_FrameIntPeriod = 69888;
	H.m_InitSP = 0xFF00;
	const QString Path = Dir.filePath("isrs.btr");
	const QString SldPath = Dir.filePath("isrs.sld");
	Save(SldPath, "|SLD.data.version|1\n||Z|pages.size:8192\ntest.asm|1||0|5|40960|F|FrameHandler\n");
	QByteArray Data;
	Put(Data, H);
	Put(Data, uint16_t(0));
	const auto SldBytes = SldPath.toLocal8Bit();
	Put(Data, uint16_t(SldBytes.size()));
	Data.append(SldBytes);
	const uint32_t Crc = Crc32(Data.constData(), size_t(Data.size()));
	Put(Data, Crc);
	const uint64_t BodyOffset = uint64_t(Data.size());
	uint64_t Previous = 0;
	auto Record = [&](uint64_t At, uint8_t Tag, uint8_t Op, uint8_t Marker, uint16_t Pc, uint16_t PostPc, uint16_t Sp,
					  uint8_t Page = 5)
	{
		Put(Data, Tag);
		Var(Data, At - Previous);
		Previous = At;
		Put(Data, Pc);
		Put(Data, Page);
		Put(Data, Op);
		Put(Data, Marker);
		Put(Data, uint16_t(0));
		Put(Data, uint16_t(kRegDiffBitDiffPc | kRegDiffBitDiffSp));
		Put(Data, PostPc);
		Put(Data, Sp);
		Put(Data, uint8_t(0));
	};
	// A 100-cycle classic frame ISR with a nested call. Caller page differs.
	Record(10, kRecord_Interrupt, 255, kIntAckMarker, 0x8000, 0xA000, 0xFEFE, 4);
	Record(20, kRecord_Instr, 0xCD, 0, 0xA000, 0xA080, 0xFEFC);
	Record(60, kRecord_Instr, 0xC9, 0, 0xA080, 0xA003, 0xFEFE);
	Record(110, kRecord_Instr, 0xC9, 0, 0xA003, 0x8000, 0xFF00);
	// A hardware-vectored frame ISR: source field must fold onto the same class.
	Record(200, kRecord_Interrupt, uint8_t(kIntSrc_Ula << 1), kIntAckMarker, 0x8000, 0xA000, 0xFEFE);
	Record(1100, kRecord_Instr, 0xC9, 0, 0xA000, 0x8000, 0xFF00);
	// Larger scanline and legacy unknown-source events must not enter the ranking.
	Record(1200, kRecord_Interrupt, uint8_t(kIntSrc_Line << 1), kIntAckMarker, 0x8000, 0xA000, 0xFEFE);
	Record(2200, kRecord_Instr, 0xC9, 0, 0xA000, 0x8000, 0xFF00);
	Record(2300, kRecord_Interrupt, 0, 0, 0x8000, 0xA000, 0xFEFE);
	Record(3400, kRecord_Instr, 0xC9, 0, 0xA000, 0x8000, 0xFF00);
	Record(3500, kRecord_Interrupt, 255, kIntAckMarker, 0x8000, 0xA000, 0xFEFE);
	Record(3510, kRecord_Instr, 0, 0, 0xA000, 0xA001, 0xFEFE); // partial at end
	Save(Path, Data);
	IndexHeader IH{};
	IH.m_Magic = kIndexMagic;
	IH.m_Version = kVersion;
	IH.m_ParentMagic = kMagic;
	IH.m_ParentHeaderCrc = Crc;
	IH.m_EntryCount = 1;
	IndexEntry IE{};
	IE.m_FirstByteOffset = BodyOffset;
	IE.SP = H.m_InitSP;
	QByteArray Idx;
	Put(Idx, IH);
	Put(Idx, IE);
	Save(Path + ".fidx", Idx);
	TraceModel Model;
	Check(Model.Open(Path).isEmpty(), "load fixture");
	Model.ComputeMetrics();
	// 810 capped active cycles in this recorded 4000-cycle span. The
	// header's initial 69888-cycle period is deliberately different.
	Check(std::abs(Model.CpuUtilisationPerFrame()[0] - 810.0f / 4000.0f) < 0.00001f,
		"utilisation uses recorded frame span rather than initial clock");
	const auto& Frames = Model.CallGraph().Frames();
	Check(Frames.size() == 6, "nested calls reconstructed");
	Check(Frames[0].m_bComplete && !Frames.last().m_bComplete, "partial ISR distinguished");
	Check(Frames[0].m_TargetPageTag == 5, "callee page used for symbols");
	InterruptView View(&Model);
	View.Refresh();
	auto* Table = View.findChild<QTreeWidget*>();
	Check(Table && Table->topLevelItemCount() == 2, "only completed frame ISRs included");
	Check(Table->topLevelItem(0)->text(2) == "900", "longest first");
	Check(Table->topLevelItem(0)->text(4) == "FrameHandler", "SLD label resolved");
	int Selected = -1;
	QObject::connect(&View, &InterruptView::ExecutionRequested,
		[&](int I)
		{
			Selected = I;
		});
	View.FocusResults();
	Check(Selected == 2, "keyboard entry selects longest ISR");
	Table->setCurrentItem(Table->topLevelItem(1));
	Check(Selected == 0, "row selection navigates");
	Table->sortItems(2, Qt::AscendingOrder);
	Check(Table->topLevelItem(0)->text(2) == "100", "numeric sorting");
	IcicleWidget Graph;
	Graph.SetModel(&Model);
	Graph.resize(1000, 350);
	Graph.show();
	uint64_t Left = 0, Right = 0;
	QObject::connect(&Graph, &IcicleWidget::ViewportChanged,
		[&](uint64_t L, uint64_t R)
		{
			Left = L;
			Right = R;
		});
	App.processEvents();
	Graph.FitExecution(2);
	Check(Left <= 200 && Right >= 1100 && Right - Left < 1100, "fit includes full ISR with padding");
	Graph.resize(600, 350);
	App.processEvents();
	Check(Left <= 200 && Right >= 1100, "fit survives window resize");
	TraceLaunchDialog Dialog;
	QVariantMap Params;
	Params["binary"] = Dir.filePath("test.nex");
	Params["sld"] = SldPath;
	Params["traceFromSeconds"] = 15;
	Params["runVal"] = 80;
	Dialog.ApplyParams(Params);
	const auto RoundTrip = Dialog.CurrentParams();
	Check(RoundTrip["sld"].toString() == SldPath && RoundTrip["traceFromSeconds"].toInt() == 15,
		"capture settings survive history round trip");
	puts("PASS: ISR sources, completion, nesting, SLD, numeric ranking, navigation, fit/resize, capture history");
}
