#include "metrics.h"

#include "bmp.h"
#include "frame_path.h"
#include "output_paths.h"

#include <math.h>
#include <string.h>
#include <filesystem>

namespace
{

constexpr int kPaperW = 256;
constexpr int kPaperH = 192;

// Pearson correlation of two equal-length byte images. Returns 0 when either
// side is flat (a uniform frame correlates with nothing meaningfully).
double Correlate(const std::vector<uint8_t>& A, const std::vector<uint8_t>& B)
{
	if (A.size() != B.size() || A.empty())
	{
		return 0.0;
	}
	double Sa = 0.0, Sb = 0.0;
	for (size_t IdxI = 0; IdxI < A.size(); ++IdxI)
	{
		Sa += A[IdxI];
		Sb += B[IdxI];
	}
	const double Ma = Sa / double(A.size()), Mb = Sb / double(B.size());
	double Num = 0.0, Da = 0.0, Db = 0.0;
	for (size_t IdxI = 0; IdxI < A.size(); ++IdxI)
	{
		const double SampleA = double(A[IdxI]) - Ma, SampleB = double(B[IdxI]) - Mb;
		Num += SampleA * SampleB;
		Da += SampleA * SampleA;
		Db += SampleB * SampleB;
	}
	if (Da <= 0.0 || Db <= 0.0)
	{
		return 0.0;
	}
	return Num / sqrt(Da * Db);
}

double MeanAbsDelta(const std::vector<uint8_t>& A, const std::vector<uint8_t>& B)
{
	if (A.size() != B.size() || A.empty())
	{
		return 0.0;
	}
	double S = 0.0;
	for (size_t IdxI = 0; IdxI < A.size(); ++IdxI)
	{
		S += abs(int(A[IdxI]) - int(B[IdxI]));
	}
	return S / double(A.size());
}

// Write an W x H greyscale image as a 24-bit bottom-up BMP. Small and
// self-contained: the point is that both machines emit the same shape of file.
bool WriteGreyBmp(const char* Path, const uint8_t* Pix, int W, int H)
{
	const int RowBytes = ((W * 3 + 3) / 4) * 4;
	const int ImageSz = RowBytes * H;
	const int Offset = 14 + 40;
	uint8_t Hdr[54] = {};
	auto U32 = [](uint8_t* P, uint32_t V)
	{
		P[0] = uint8_t(V);
		P[1] = uint8_t(V >> 8);
		P[2] = uint8_t(V >> 16);
		P[3] = uint8_t(V >> 24);
	};
	auto U16 = [](uint8_t* P, uint16_t V)
	{
		P[0] = uint8_t(V);
		P[1] = uint8_t(V >> 8);
	};
	Hdr[0] = 'B';
	Hdr[1] = 'M';
	U32(Hdr + 2, uint32_t(Offset + ImageSz));
	U32(Hdr + 10, uint32_t(Offset));
	U32(Hdr + 14, 40);
	U32(Hdr + 18, uint32_t(W));
	U32(Hdr + 22, uint32_t(H));
	U16(Hdr + 26, 1);
	U16(Hdr + 28, 24);
	U32(Hdr + 34, uint32_t(ImageSz));
	U32(Hdr + 38, 2835);
	U32(Hdr + 42, 2835);
	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		return false;
	}
	bool bOk = fwrite(Hdr, 1, sizeof(Hdr), F) == sizeof(Hdr);
	std::vector<uint8_t> Row(size_t(RowBytes), 0);
	for (int Y = H - 1; Y >= 0 && bOk; --Y) // BMP rows are bottom-up
	{
		for (int X = 0; X < W; ++X)
		{
			const uint8_t V = Pix[size_t(Y) * size_t(W) + size_t(X)];
			Row[size_t(X) * 3 + 0] = V;
			Row[size_t(X) * 3 + 1] = V;
			Row[size_t(X) * 3 + 2] = V;
		}
		bOk = fwrite(Row.data(), 1, Row.size(), F) == Row.size();
	}
	fclose(F);
	return bOk;
}

} // namespace

bool OpenMetrics(MetricsCapture& InOutM, std::string& OutError)
{
	if (InOutM.m_Block < 1)
	{
		InOutM.m_Block = 1;
	}
	if (!InOutM.m_MotionPath.empty())
	{
		InOutM.m_MotionFile = fopen(InOutM.m_MotionPath.c_str(), "wb");
		if (InOutM.m_MotionFile == nullptr)
		{
			OutError = "cannot open motion output: " + InOutM.m_MotionPath;
			return false;
		}
		fprintf(InOutM.m_MotionFile,
			"# inspeccy motion, 256x192 paper area, %dx%d block average\n"
			"frame,mean_abs_delta,corr_1,corr_4,corr_16\n",
			InOutM.m_Block, InOutM.m_Block);
	}
	if (!InOutM.m_HistPath.empty())
	{
		InOutM.m_HistFile = fopen(InOutM.m_HistPath.c_str(), "wb");
		if (InOutM.m_HistFile == nullptr)
		{
			OutError = "cannot open histogram output: " + InOutM.m_HistPath;
			return false;
		}
		fprintf(InOutM.m_HistFile,
			"# inspeccy brightness histogram, 256x192 paper area, %dx%d block average\n"
			"frame,mean",
			InOutM.m_Block, InOutM.m_Block);
		for (int B = 0; B < 16; ++B)
		{
			fprintf(InOutM.m_HistFile, ",b%d", B);
		}
		fprintf(InOutM.m_HistFile, "\n");
	}
	InOutM.m_Luma.assign(size_t(kPaperW) * size_t(kPaperH), 0);
	return true;
}

bool EmitMetricsFrame(MetricsCapture& InOutM, const MemoryBus& Mem, const SpritesState* Sprites, std::string& OutError)
{
	// Advance the deadline first: a render or write failure must not trap the
	// dispatcher in a per-instruction retry.
	++InOutM.m_FrameCounter;
	InOutM.m_NextEmitCycle += InOutM.m_PeriodCycles;

	if (!RenderPaperLuma(Mem, Sprites, InOutM.m_Luma.data(), OutError))
	{
		return false;
	}

	// Downsample by Block x Block (box average).
	const int Bw = kPaperW / InOutM.m_Block, Bh = kPaperH / InOutM.m_Block;
	InOutM.m_Small.assign(size_t(Bw) * size_t(Bh), 0);
	if (InOutM.m_Block == 1)
	{
		memcpy(InOutM.m_Small.data(), InOutM.m_Luma.data(), InOutM.m_Small.size());
	}
	else
	{
		for (int Y = 0; Y < Bh; ++Y)
		{
			for (int X = 0; X < Bw; ++X)
			{
				unsigned Sum = 0;
				for (int Dy = 0; Dy < InOutM.m_Block; ++Dy)
				{
					for (int Dx = 0; Dx < InOutM.m_Block; ++Dx)
					{
						Sum +=
							InOutM.m_Luma[size_t(Y * InOutM.m_Block + Dy) * kPaperW + size_t(X * InOutM.m_Block + Dx)];
					}
				}
				InOutM.m_Small[size_t(Y) * size_t(Bw) + size_t(X)] =
					uint8_t(Sum / unsigned(InOutM.m_Block * InOutM.m_Block));
			}
		}
	}

	if (InOutM.m_HistFile != nullptr)
	{
		unsigned Bin[16] = {};
		double Sum = 0.0;
		for (uint8_t V : InOutM.m_Small)
		{
			Bin[V >> 4]++;
			Sum += V;
		}
		fprintf(InOutM.m_HistFile, "%llu,%.2f", static_cast<unsigned long long>(InOutM.m_FrameCounter - 1),
			Sum / double(InOutM.m_Small.size()));
		for (int B = 0; B < 16; ++B)
		{
			fprintf(InOutM.m_HistFile, ",%.5f", double(Bin[B]) / double(InOutM.m_Small.size()));
		}
		fprintf(InOutM.m_HistFile, "\n");
	}

	if (InOutM.m_MotionFile != nullptr)
	{
		const size_t N = InOutM.m_History.size();
		auto Lag = [&](size_t K) -> double
		{
			return (N >= K) ? Correlate(InOutM.m_History[N - K], InOutM.m_Small) : 0.0;
		};
		const double D = (N >= 1) ? MeanAbsDelta(InOutM.m_History[N - 1], InOutM.m_Small) : 0.0;
		fprintf(InOutM.m_MotionFile, "%llu,%.4f,%.4f,%.4f,%.4f\n",
			static_cast<unsigned long long>(InOutM.m_FrameCounter - 1), D, Lag(1), Lag(4), Lag(16));
	}

	if (!InOutM.m_PaperPattern.empty())
	{
		std::string Path;
		if (FormatFramePath(InOutM.m_PaperPattern, InOutM.m_FrameCounter - 1, Path))
		{
			if (InOutM.m_OutputPaths && !InOutM.m_OutputPaths->CheckFrame(Path, InOutM.m_PaperPattern, OutError))
			{
				return false;
			}
			if (InOutM.m_FrameCounter == 1)
			{
				std::error_code Ec;
				const std::filesystem::path Parent = std::filesystem::path(Path).parent_path();
				if (!Parent.empty())
				{
					std::filesystem::create_directories(Parent, Ec);
				}
			}
			if (!WriteGreyBmp(Path.c_str(), InOutM.m_Small.data(), Bw, Bh))
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	InOutM.m_History.push_back(InOutM.m_Small);
	if (InOutM.m_History.size() > 17)
	{
		InOutM.m_History.erase(InOutM.m_History.begin());
	}
	InOutM.m_bAny = true;
	return true;
}

void CloseMetrics(MetricsCapture& InOutM)
{
	if (InOutM.m_MotionFile != nullptr)
	{
		const bool bError = ferror(InOutM.m_MotionFile) != 0;
		if (fclose(InOutM.m_MotionFile) != 0 || bError)
		{
			InOutM.m_bWriteFailed = true;
		}
		InOutM.m_MotionFile = nullptr;
	}
	if (InOutM.m_HistFile != nullptr)
	{
		const bool bError = ferror(InOutM.m_HistFile) != 0;
		if (fclose(InOutM.m_HistFile) != 0 || bError)
		{
			InOutM.m_bWriteFailed = true;
		}
		InOutM.m_HistFile = nullptr;
	}
}
