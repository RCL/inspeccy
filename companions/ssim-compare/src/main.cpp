// Structural comparison of two numbered BMP frame sequences, by greyscale
// SSIM. A companion to inspeccy rather than part of it: it reads only files,
// knows nothing about a Spectrum, and links nothing.
//
// Scores and thresholds are diagnostics, not proof of visual or hardware
// accuracy. Colour differences disappear in the greyscale conversion, and a
// short overlap can give a misleading alignment score. Look at the frames.
//
// Exit codes: 0 pass, 1 below --min-ssim, 2 invalid input or no usable
// overlap.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

int Fail(const std::string& Message)
{
	fprintf(stderr, "ssim-compare: %s\n", Message.c_str());
	return 2;
}

// ------------------------------------------------------------------- BMP
// Enough of BI_RGB to read what inspeccy writes: 4, 8 and 24 bits per pixel,
// bottom-up or top-down. Greyscale conversion matches the reference
// implementation's integer luma so the two tools' scores are comparable.
struct Image
{
	int Width = 0, Height = 0;
	std::vector<float> Grey; // row-major, top-down
};

uint32_t ReadU32(const uint8_t* P)
{
	return uint32_t(P[0]) | (uint32_t(P[1]) << 8) | (uint32_t(P[2]) << 16) | (uint32_t(P[3]) << 24);
}

uint16_t ReadU16(const uint8_t* P)
{
	return uint16_t(uint16_t(P[0]) | (uint16_t(P[1]) << 8));
}

// Rec.601 luma, rounded, matching the reference greyscale conversion; a grey
// palette entry maps back to itself exactly.
uint8_t Luma(uint8_t R, uint8_t G, uint8_t B)
{
	return uint8_t((int32_t(R) * 299 + int32_t(G) * 587 + int32_t(B) * 114 + 500) / 1000);
}

bool LoadBmp(const std::string& Path, Image& Out, std::string& OutError)
{
	FILE* F = fopen(Path.c_str(), "rb");
	if (F == nullptr)
	{
		OutError = "cannot read " + Path;
		return false;
	}
	std::vector<uint8_t> Bytes;
	uint8_t Chunk[65536];
	size_t Got = 0;
	while ((Got = fread(Chunk, 1, sizeof(Chunk), F)) > 0)
	{
		Bytes.insert(Bytes.end(), Chunk, Chunk + Got);
		if (Bytes.size() > (64u << 20))
		{
			break;
		}
	}
	fclose(F);

	if (Bytes.size() < 54 || Bytes[0] != 'B' || Bytes[1] != 'M')
	{
		OutError = Path + ": not a BMP";
		return false;
	}
	const uint32_t DataAt = ReadU32(&Bytes[10]);
	const uint32_t HeaderSize = ReadU32(&Bytes[14]);
	if (HeaderSize < 40 || 14 + HeaderSize > Bytes.size())
	{
		OutError = Path + ": unsupported BMP header";
		return false;
	}
	const int32_t W = int32_t(ReadU32(&Bytes[18]));
	const int32_t RawH = int32_t(ReadU32(&Bytes[22]));
	const uint16_t Bits = ReadU16(&Bytes[28]);
	const uint32_t Compression = ReadU32(&Bytes[30]);
	const bool bTopDown = RawH < 0;
	const int32_t H = bTopDown ? -RawH : RawH;
	if (Compression != 0 || W <= 0 || H <= 0 || W > 65535 || H > 65535)
	{
		OutError = Path + ": only uncompressed BMPs of a sane size are supported";
		return false;
	}
	if (Bits != 4 && Bits != 8 && Bits != 24)
	{
		OutError = Path + ": unsupported bit depth " + std::to_string(Bits);
		return false;
	}

	uint8_t Palette[256] = {};
	if (Bits <= 8)
	{
		uint32_t Colours = ReadU32(&Bytes[46]);
		if (Colours == 0 || Colours > (1u << Bits))
		{
			Colours = 1u << Bits;
		}
		const size_t At = 14 + HeaderSize;
		if (At + Colours * 4 > Bytes.size())
		{
			OutError = Path + ": truncated colour table";
			return false;
		}
		for (uint32_t IdxI = 0; IdxI < Colours; ++IdxI)
		{
			const uint8_t* E = &Bytes[At + IdxI * 4];
			Palette[IdxI] = Luma(E[2], E[1], E[0]);
		}
	}

	const size_t Stride = ((size_t(W) * Bits + 31) / 32) * 4;
	if (DataAt + Stride * size_t(H) > Bytes.size())
	{
		OutError = Path + ": truncated pixel data";
		return false;
	}
	Out.Width = W;
	Out.Height = H;
	Out.Grey.assign(size_t(W) * size_t(H), 0.0f);
	for (int32_t Y = 0; Y < H; ++Y)
	{
		const uint8_t* Row = &Bytes[DataAt + Stride * size_t(bTopDown ? Y : H - 1 - Y)];
		float* Dest = &Out.Grey[size_t(Y) * size_t(W)];
		for (int32_t X = 0; X < W; ++X)
		{
			if (Bits == 24)
			{
				Dest[X] = float(Luma(Row[X * 3 + 2], Row[X * 3 + 1], Row[X * 3]));
			}
			else if (Bits == 8)
			{
				Dest[X] = float(Palette[Row[X]]);
			}
			else
			{
				const uint8_t Pair = Row[X / 2];
				Dest[X] = float(Palette[(X & 1) ? (Pair & 15) : (Pair >> 4)]);
			}
		}
	}
	return true;
}

// ------------------------------------------------------------------ SSIM
// Means over a k x k window, "valid" region only, from a summed-area table.
// The table is accumulated down the columns first and along the rows second,
// which vectorises across x and matches the reference's accumulation order.
void SummedArea(const float* Src, int W, int H, std::vector<double>& Out)
{
	Out.assign(size_t(W + 1) * size_t(H + 1), 0.0);
	for (int Y = 0; Y < H; ++Y)
	{
		const float* In = Src + size_t(Y) * size_t(W);
		const double* Above = &Out[size_t(Y) * size_t(W + 1) + 1];
		double* Row = &Out[size_t(Y + 1) * size_t(W + 1) + 1];
#pragma omp simd
		for (int X = 0; X < W; ++X)
		{
			Row[X] = Above[X] + double(In[X]);
		}
	}
	for (int Y = 1; Y <= H; ++Y)
	{
		double* Row = &Out[size_t(Y) * size_t(W + 1)];
		double Running = 0.0;
		for (int X = 1; X <= W; ++X)
		{
			Running += Row[X];
			Row[X] = Running;
		}
	}
}

// Box means of the valid region, written to Out ((W-k+1) x (H-k+1)).
void BoxMean(const std::vector<double>& Sat, int W, int H, int K, std::vector<double>& Out)
{
	const int OW = W - K + 1, OH = H - K + 1;
	const double Inv = 1.0 / (double(K) * double(K));
	Out.resize(size_t(OW) * size_t(OH));
	for (int Y = 0; Y < OH; ++Y)
	{
		const double* Top = &Sat[size_t(Y) * size_t(W + 1)];
		const double* Bottom = &Sat[size_t(Y + K) * size_t(W + 1)];
		double* Dest = &Out[size_t(Y) * size_t(OW)];
#pragma omp simd
		for (int X = 0; X < OW; ++X)
		{
			Dest[X] = (Bottom[X + K] - Top[X + K] - Bottom[X] + Top[X]) * Inv;
		}
	}
}

// Scratch buffers, reused across every frame pair: the sequences are long
// and the allocations would otherwise dominate.
struct SsimWorkspace
{
	std::vector<float> Product;
	std::vector<double> Sat, MuA, MuB, SaA, SbB, SaB;
};

double Ssim(const Image& A, const Image& B, int K, SsimWorkspace& Work)
{
	const int W = A.Width, H = A.Height;
	K = std::min(K, std::min(W, H));
	const int OW = W - K + 1, OH = H - K + 1;
	const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
	const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
	const size_t N = size_t(W) * size_t(H);

	SummedArea(A.Grey.data(), W, H, Work.Sat);
	BoxMean(Work.Sat, W, H, K, Work.MuA);
	SummedArea(B.Grey.data(), W, H, Work.Sat);
	BoxMean(Work.Sat, W, H, K, Work.MuB);

	Work.Product.resize(N);
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		Work.Product[IdxI] = A.Grey[IdxI] * A.Grey[IdxI];
	}
	SummedArea(Work.Product.data(), W, H, Work.Sat);
	BoxMean(Work.Sat, W, H, K, Work.SaA);
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		Work.Product[IdxI] = B.Grey[IdxI] * B.Grey[IdxI];
	}
	SummedArea(Work.Product.data(), W, H, Work.Sat);
	BoxMean(Work.Sat, W, H, K, Work.SbB);
	for (size_t IdxI = 0; IdxI < N; ++IdxI)
	{
		Work.Product[IdxI] = A.Grey[IdxI] * B.Grey[IdxI];
	}
	SummedArea(Work.Product.data(), W, H, Work.Sat);
	BoxMean(Work.Sat, W, H, K, Work.SaB);

	const double* __restrict MuA = Work.MuA.data();
	const double* __restrict MuB = Work.MuB.data();
	const double* __restrict SaA = Work.SaA.data();
	const double* __restrict SbB = Work.SbB.data();
	const double* __restrict SaB = Work.SaB.data();
	const size_t Count = size_t(OW) * size_t(OH);
	double Sum = 0.0;
#pragma omp simd reduction(+ : Sum)
	for (size_t IdxI = 0; IdxI < Count; ++IdxI)
	{
		const double Ma = MuA[IdxI], Mb = MuB[IdxI];
		const double Vaa = SaA[IdxI] - Ma * Ma;
		const double Vbb = SbB[IdxI] - Mb * Mb;
		const double Vab = SaB[IdxI] - Ma * Mb;
		const double Num = (2.0 * Ma * Mb + C1) * (2.0 * Vab + C2);
		const double Den = (Ma * Ma + Mb * Mb + C1) * (Vaa + Vbb + C2);
		Sum += Num / Den;
	}
	return Sum / double(Count);
}

// ------------------------------------------------------------ the command
struct Options
{
	std::string DirA, DirB;
	long Offset = 0, SearchRange = 600, SearchStep = 1, Sample = 60, Window = 8, Worst = 0,
		 Windows = 0;
	bool bSearch = false, bHaveMinSsim = false;
	double MinSsim = 0.0;
};

bool ParseLong(const char* Text, long& Out)
{
	char* End = nullptr;
	const long V = strtol(Text, &End, 10);
	if (End == Text || *End != '\0')
	{
		return false;
	}
	Out = V;
	return true;
}

bool ParseDouble(const char* Text, double& Out)
{
	char* End = nullptr;
	const double V = strtod(Text, &End);
	if (End == Text || *End != '\0')
	{
		return false;
	}
	Out = V;
	return true;
}

bool LoadDir(const std::string& Path, std::vector<Image>& Out, std::string& OutError)
{
	namespace fs = std::filesystem;
	std::error_code Ec;
	std::vector<std::string> Files;
	for (const fs::directory_entry& E : fs::directory_iterator(Path, Ec))
	{
		if (E.is_regular_file(Ec) && E.path().extension() == ".bmp")
		{
			Files.push_back(E.path().string());
		}
	}
	if (Ec || Files.empty())
	{
		OutError = "no .bmp frames in " + Path;
		return false;
	}
	std::sort(Files.begin(), Files.end());
	Out.reserve(Files.size());
	for (const std::string& File : Files)
	{
		Image Frame;
		if (!LoadBmp(File, Frame, OutError))
		{
			return false;
		}
		Out.push_back(std::move(Frame));
	}
	return true;
}

// SSIM of every overlapping pair at a fixed offset (B[i + Offset] vs A[i]).
void Series(const std::vector<Image>& A, const std::vector<Image>& B, long Offset, int K,
	SsimWorkspace& Work, std::vector<std::pair<long, double>>& Out)
{
	const long Lo = std::max(0L, -Offset);
	const long Hi = std::min(long(A.size()), long(B.size()) - Offset);
	for (long IdxI = Lo; IdxI < Hi; ++IdxI)
	{
		Out.push_back({IdxI, Ssim(A[size_t(IdxI)], B[size_t(IdxI + Offset)], K, Work)});
	}
}

} // namespace

int main(int Argc, char** Argv)
{
	Options Opt;
	std::vector<std::string> Positional;
	for (int IdxI = 1; IdxI < Argc; ++IdxI)
	{
		const std::string A = Argv[IdxI];
		auto Value = [&]() -> const char*
		{
			return (IdxI + 1 < Argc) ? Argv[++IdxI] : nullptr;
		};
		auto Number = [&](long& Dest)
		{
			const char* V = Value();
			return V != nullptr && ParseLong(V, Dest);
		};
		bool bOk = true;
		if (A == "--search")
		{
			Opt.bSearch = true;
		}
		else if (A == "--offset")
		{
			bOk = Number(Opt.Offset);
		}
		else if (A == "--search-range")
		{
			bOk = Number(Opt.SearchRange);
		}
		else if (A == "--search-step")
		{
			bOk = Number(Opt.SearchStep);
		}
		else if (A == "--sample")
		{
			bOk = Number(Opt.Sample);
		}
		else if (A == "--window")
		{
			bOk = Number(Opt.Window);
		}
		else if (A == "--worst")
		{
			bOk = Number(Opt.Worst);
		}
		else if (A == "--windows")
		{
			bOk = Number(Opt.Windows);
		}
		else if (A == "--min-ssim")
		{
			const char* V = Value();
			bOk = V != nullptr && ParseDouble(V, Opt.MinSsim);
			Opt.bHaveMinSsim = true;
		}
		else if (A == "--help" || A == "-h")
		{
			puts("usage: ssim-compare DIR_A DIR_B [--offset N] [--search] "
				 "[--search-range N]\n"
				 "                    [--search-step N] [--sample N] [--window N] "
				 "[--worst N]\n"
				 "                    [--windows N] [--min-ssim X]");
			return 0;
		}
		else if (!A.empty() && A[0] == '-')
		{
			return Fail("unknown option " + A);
		}
		else
		{
			Positional.push_back(A);
			continue;
		}
		if (!bOk)
		{
			return Fail(A + " needs a numeric value");
		}
	}
	if (Positional.size() != 2)
	{
		return Fail("two frame directories are required");
	}
	Opt.DirA = Positional[0];
	Opt.DirB = Positional[1];

	if (Opt.SearchStep < 1 || Opt.Sample < 1 || Opt.Window < 1)
	{
		return Fail("--search-step, --sample and --window must be positive");
	}
	if (std::min(Opt.SearchRange, std::min(Opt.Worst, Opt.Windows)) < 0)
	{
		return Fail("--search-range, --worst and --windows must be nonnegative");
	}
	if (Opt.bHaveMinSsim && !(std::isfinite(Opt.MinSsim) && Opt.MinSsim >= -1.0 && Opt.MinSsim <= 1.0))
	{
		return Fail("--min-ssim must be finite and in [-1, 1]");
	}

	std::vector<Image> A, B;
	std::string Error;
	if (!LoadDir(Opt.DirA, A, Error) || !LoadDir(Opt.DirB, B, Error))
	{
		return Fail(Error);
	}
	for (const std::vector<Image>* Set : {&A, &B})
	{
		for (const Image& Frame : *Set)
		{
			if (Frame.Width != A[0].Width || Frame.Height != A[0].Height)
			{
				return Fail("all frames must have the same dimensions");
			}
		}
	}
	printf("%zu frames in %s, %zu in %s, %dx%d each\n", A.size(), Opt.DirA.c_str(), B.size(),
		Opt.DirB.c_str(), A[0].Width, A[0].Height);

	SsimWorkspace Work;
	const int K = int(Opt.Window);
	long Offset = Opt.Offset;
	if (Opt.bSearch)
	{
		const long Step = std::max(1L, long(A.size()) / Opt.Sample);
		bool bFound = false;
		double BestMean = 0.0;
		long BestOffset = 0;
		for (long Off = -Opt.SearchRange; Off <= Opt.SearchRange; Off += Opt.SearchStep)
		{
			const long Lo = std::max(0L, -Off);
			const long Hi = std::min(long(A.size()), long(B.size()) - Off);
			if (Hi - Lo < 8)
			{
				continue;
			}
			double Sum = 0.0;
			long Count = 0;
			for (long IdxI = Lo; IdxI < Hi; IdxI += Step)
			{
				Sum += Ssim(A[size_t(IdxI)], B[size_t(IdxI + Off)], K, Work);
				++Count;
			}
			const double Mean = Sum / double(Count);
			if (!bFound || Mean > BestMean)
			{
				bFound = true;
				BestMean = Mean;
				BestOffset = Off;
			}
		}
		if (!bFound)
		{
			return Fail("offset search needs at least eight overlapping frames");
		}
		Offset = BestOffset;
		printf("best offset %+ld frames, mean SSIM %.4f\n", Offset, BestMean);
	}

	if (Opt.Windows != 0)
	{
		// Two runs of the same effect can share every frame and still score
		// badly on one global offset, because anything that stalls for a
		// different number of frames (a table rebuild, a loading pause) makes
		// them drift. Aligning each window separately separates "different
		// picture" from "same picture, drifted".
		const long Span = std::max(1L, long(A.size()) / Opt.Windows);
		printf("per-window alignment (%ld windows of %ld frames):\n", Opt.Windows, Span);
		std::vector<std::pair<double, long>> Best;
		for (long Lo = 0; Lo + Span <= long(A.size()); Lo += Span)
		{
			bool bLocal = false;
			double LocalMean = 0.0;
			long LocalOffset = 0;
			for (long Off = Offset - 120; Off <= Offset + 120; Off += 2)
			{
				const long Start = std::max(Lo, -Off);
				const long Hi = std::min(Lo + Span, std::min(long(A.size()), long(B.size()) - Off));
				if (Hi - Start < 20)
				{
					continue;
				}
				double Sum = 0.0;
				long Count = 0;
				for (long IdxI = Start; IdxI < Hi; IdxI += std::max(1L, Span / 20))
				{
					Sum += Ssim(A[size_t(IdxI)], B[size_t(IdxI + Off)], K, Work);
					++Count;
				}
				const double Mean = Sum / double(Count);
				if (!bLocal || Mean > LocalMean)
				{
					bLocal = true;
					LocalMean = Mean;
					LocalOffset = Off;
				}
			}
			if (bLocal)
			{
				Best.push_back({LocalMean, LocalOffset});
				printf("   A[%5ld..%5ld]  offset %+5ld   mean SSIM %.3f\n", Lo, Lo + Span,
					LocalOffset, LocalMean);
			}
		}
		if (!Best.empty())
		{
			double Sum = 0.0;
			long Low = Best[0].second, High = Best[0].second;
			for (const std::pair<double, long>& Item : Best)
			{
				Sum += Item.first;
				Low = std::min(Low, Item.second);
				High = std::max(High, Item.second);
			}
			printf("   locally aligned mean SSIM %.3f   offset drift %+ld..%+ld (%ld frames)\n",
				Sum / double(Best.size()), Low, High, High - Low);
		}
	}

	std::vector<std::pair<long, double>> S;
	Series(A, B, Offset, K, Work, S);
	if (S.empty())
	{
		return Fail("no overlapping frames at that offset");
	}
	std::vector<double> Vals;
	Vals.reserve(S.size());
	double Sum = 0.0, Above9 = 0.0, Above7 = 0.0, Below5 = 0.0;
	for (const std::pair<long, double>& Item : S)
	{
		Vals.push_back(Item.second);
		Sum += Item.second;
		Above9 += (Item.second > 0.9) ? 1.0 : 0.0;
		Above7 += (Item.second > 0.7) ? 1.0 : 0.0;
		Below5 += (Item.second < 0.5) ? 1.0 : 0.0;
	}
	const double Mean = Sum / double(Vals.size());
	std::vector<double> Sorted = Vals;
	std::sort(Sorted.begin(), Sorted.end());
	const size_t Half = Sorted.size() / 2;
	const double Median
		= (Sorted.size() % 2 != 0) ? Sorted[Half] : 0.5 * (Sorted[Half - 1] + Sorted[Half]);
	printf("offset %+ld: %zu frame pairs   mean SSIM %.4f   median %.4f   min %.4f   max %.4f\n",
		Offset, S.size(), Mean, Median, Sorted.front(), Sorted.back());
	const double N = double(Vals.size());
	printf("   frames above 0.9: %4.1f%%   above 0.7: %4.1f%%   below 0.5: %4.1f%%\n",
		100.0 * Above9 / N, 100.0 * Above7 / N, 100.0 * Below5 / N);

	if (Opt.Worst != 0)
	{
		std::vector<std::pair<long, double>> ByScore = S;
		std::stable_sort(ByScore.begin(), ByScore.end(),
			[](const std::pair<long, double>& X, const std::pair<long, double>& Y)
			{ return X.second < Y.second; });
		for (size_t IdxI = 0; IdxI < ByScore.size() && IdxI < size_t(Opt.Worst); ++IdxI)
		{
			printf("   worst: A frame %5ld vs B frame %5ld   SSIM %.4f\n", ByScore[IdxI].first,
				ByScore[IdxI].first + Offset, ByScore[IdxI].second);
		}
	}

	if (Opt.bHaveMinSsim && Mean < Opt.MinSsim)
	{
		fprintf(stderr, "ssim-compare: mean SSIM %.4f below --min-ssim %g\n", Mean, Opt.MinSsim);
		return 1;
	}
	return 0;
}
