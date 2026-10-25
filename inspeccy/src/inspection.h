#pragma once
#include "memory.h"
#include <string>
#include <vector>

struct IoContext;
struct Z80State;
struct SldSymbolMap;
struct JsonValue;

struct InspectionIdentity
{
	std::string ImageSha1, Override;
};
std::string InspectionCpuJson(const Z80State& Cpu);
std::string InspectionIdentityJson(const InspectionIdentity& Identity, bool BoundSymbols);
std::string InspectionAddressJson(uint16_t Address, const DebugMapping& Mapping, const SldSymbolMap& Symbols);
std::string InspectionStateJson(const Z80State& Cpu, const IoContext& Io, const SldSymbolMap& Symbols,
	bool BoundSymbols, const InspectionIdentity& Identity, unsigned StackWords = 16, unsigned PcBytes = 16);

struct DeviceQuery
{
	std::string Device, Layer, Encoding;
	unsigned Start = 0, Count = 0, Bank = 0, Chip = 0, Slot = 0;
	bool Raw = false;
	std::vector<uint16_t> Expected;
};
DeviceQuery ParseDeviceQuery(const JsonValue& Step, const IoContext& Io, bool Expect);
std::vector<uint16_t> InspectDevice(const DeviceQuery& Q, const IoContext& Io);
std::string DeviceSelectorJson(const DeviceQuery& Q);
std::string DeviceResultJson(const DeviceQuery& Q, const IoContext& Io, const std::vector<uint16_t>& Values);
std::string InspectionCapabilitiesJson();
