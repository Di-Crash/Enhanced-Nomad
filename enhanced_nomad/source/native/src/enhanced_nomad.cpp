#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

static bool Initialize();
static bool SetPower(std::uint64_t ship, float weapons, float shields, float engines);

namespace
{
	struct LuaState;
	struct SpeedInfo;

	struct GroupPowerState
	{
		float weapons;
		float shields;
	};

	struct ShieldScale
	{
		void* group;
		float previous;
	};

	struct ImageRange
	{
		std::uint8_t* data;
		std::size_t size;
	};

	using LuaHandler = int(__cdecl*)(LuaState*);
	using LuaBool = void(__cdecl*)(LuaState*, int);
	using LuaString = void(__cdecl*)(LuaState*, const char*, std::size_t);
	using LuaText = const char* (__cdecl*)(LuaState*, int, std::size_t*);
	using LuaTop = int(__cdecl*)(LuaState*);
	using LuaNumber = double(__cdecl*)(LuaState*, int);
	using LuaIsNumber = int(__cdecl*)(LuaState*, int);
	using LuaToBool = int(__cdecl*)(LuaState*, int);
	using LuaPushCClosure = void(__cdecl*)(LuaState*, LuaHandler, int);
	using LuaCreateTable = void(__cdecl*)(LuaState*, int, int);
	using LuaSetField = void(__cdecl*)(LuaState*, int, const char*);

	using ResolveObj = void* (__fastcall*)(void*, std::uint64_t, std::uint32_t);

	using MoveCalc = void(__fastcall*)(void*, void*, const void*, void*);
	using TheoreticalMoveCalc = void(__fastcall*)(void*, const void*, void*, void*, void*);
	using GetDefensibleSpeeds = SpeedInfo* (__fastcall*)(SpeedInfo*, std::uint64_t);
	using MoveApply = void(__fastcall*)(void*, void*, std::uint8_t, double, std::uint8_t, void*);

	using ShieldAdd = void(__fastcall*)(void*, void*);
	using ShieldRead = double(__fastcall*)(void*, double);
	using ShieldSet = double(__fastcall*)(void*, double, double);
	using WeaponReload = float(__fastcall*)(void*);

	using TradeAccountHolder = void* (__fastcall*)(void*, void*, bool);
	using MapInfoBuilder = void(__fastcall*)(void*, void*, bool, void*, void*, void*);
	using MapInfoEligibility = bool(__fastcall*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, void*, void*);

	using ContainerBuildResourceCollector = void(__fastcall*)(void*, void*, void*, void*, void*, bool);
	using EquipmentBuildResourceCollector = void(__fastcall*)(void*);
	using ContainerTypeCheck = bool(__fastcall*)(void*, std::uint32_t);
	using ContainerIsStation = bool(__fastcall*)(void*);

	constexpr unsigned supportedGameVersion = 900;
	constexpr unsigned supportedGameBuild = 611726;
	constexpr std::string_view version = "1.0.0";
	constexpr std::string_view speedName = "GetDefensibleSpeeds";
	constexpr std::string_view nomadMacro = "en_ship_arg_xl_resupplier_macro";

	constexpr std::size_t componentId = 0x08;
	constexpr std::size_t componentDefinition = 0x30;

	constexpr std::size_t moveSize = 0x140;
	constexpr std::size_t moveContext = 0x300;

	constexpr std::size_t shieldCapacity = 0x40;
	constexpr std::size_t shieldState = 0x58;
	constexpr std::size_t shieldLink = 0x60;
	constexpr std::size_t shieldGroup = 0x290;
	constexpr std::size_t shipGroupsBegin = 0x5D8;
	constexpr std::size_t shipGroupsEnd = 0x5E0;

	constexpr std::size_t ownAccountState = 0x708;
	constexpr std::size_t tradeFilterEntriesPrimary = 0x8F8;
	constexpr std::size_t tradeFilterEntriesSecondary = 0x910;

	HMODULE module;
	std::filesystem::path logPath;
	std::string initFailureKind;
	std::mutex logLock;
	std::mutex initLock;
	std::atomic_bool ready{ false };

	LuaText luaText;
	LuaTop luaTop;
	LuaNumber luaNumber;
	LuaIsNumber luaIsNumber;
	LuaToBool luaToBool;
	LuaBool luaPushBool;
	LuaPushCClosure luaPushCClosure;
	LuaCreateTable luaCreateTable;
	LuaSetField luaSetField;

	ResolveObj resolveObj;
	void** universe;

	std::shared_mutex powerLock;
	std::unordered_map<std::uint64_t, float> enginePower;
	std::unordered_map<void*, GroupPowerState> groupPower;
	std::unordered_map<std::uint64_t, std::vector<void*>> shipGroups;
	MoveCalc moveCall;
	TheoreticalMoveCalc theoreticalMoveCall;
	GetDefensibleSpeeds getDefensibleSpeedsCall;
	MoveApply applyMove;
	thread_local float speedQueryFactor = 1.0F;
	ShieldAdd shieldCall;
	ShieldSet setShield;
	WeaponReload weaponReloadCall;

	std::mutex mapInfoLock;
	std::unordered_map<std::uint64_t, bool> mapInfoVisible;
	std::atomic_bool mapTradeEnabled{ true };
	TradeAccountHolder tradeAccountCall;
	std::uint8_t* tradeBudgetReturn;
	MapInfoBuilder mapInfoCall;
	MapInfoEligibility mapInfoEligibilityCall;
	std::uint8_t* mapInfoEligibilityReturn;
	std::uint8_t* tradeFilterEligibilityReturn;

	std::mutex buildResourceLock;
	ContainerBuildResourceCollector containerBuildResourceCall;
	EquipmentBuildResourceCollector equipmentBuildResourceCall;
	ContainerTypeCheck containerTypeCheckCall;
	void** containerTypeCheckSlot;
	ContainerIsStation containerIsStationCall;
	void** containerIsStationSlot;
	thread_local void* nomadBuildResourceContainer = nullptr;
	thread_local void* nomadEquipmentResourceContainer = nullptr;

	std::filesystem::path GetLogPath()
	{
		std::wstring path(32768, L'\0');
		const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));

		if (length == 0 or length == path.size())
		{
			return {};
		}

		path.resize(length);

		return std::filesystem::path(path).parent_path() / "EnhancedNomad.log";
	}

	void StartLog()
	{
		std::lock_guard guard(logLock);

		if (logPath.empty())
		{
			return;
		}

		std::ofstream stream(logPath, std::ios::trunc);

		if (!stream)
		{
			return;
		}

		SYSTEMTIME time{};
		GetLocalTime(&time);
		stream << "Enhanced Nomad Native Log\n"
			<< "Version: " << version << '\n'
			<< "Started: " << std::setfill('0') << std::setw(4) << time.wYear << '-'
			<< std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay << ' '
			<< std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
			<< std::setw(2) << time.wSecond << "\n\n";
	}

	void WriteLog(std::string_view source, std::string_view text)
	{
		std::lock_guard guard(logLock);

		if (logPath.empty())
		{
			return;
		}

		std::ofstream stream(logPath, std::ios::app);

		if (!stream)
		{
			return;
		}

		stream << "[Enhanced Nomad][" << source << "] " << text << '\n';
	}

	void SetInitFailure(std::string_view kind)
	{
		initFailureKind.assign(kind);
	}

	void CheckBind(std::string_view name, bool found, bool& valid)
	{
		if (found)
		{
			return;
		}

		WriteLog("Hook", "binding missing name=" + std::string(name));
		valid = false;
	}

	bool IsPower(float value)
	{
		return std::isfinite(value) and value >= 0.0F and value <= 4.0F;
	}

	bool Match(const std::uint8_t* data, std::initializer_list<std::uint8_t> bytes)
	{
		return std::equal(bytes.begin(), bytes.end(), data);
	}

	std::int32_t ReadDisp(const std::uint8_t* data)
	{
		std::int32_t value;
		std::memcpy(&value, data, sizeof(value));

		return value;
	}

	std::uint8_t* GetTarget(const std::uint8_t* next, std::int32_t disp)
	{
		return reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::intptr_t>(next) + disp);
	}

	bool HasRange(const std::uint8_t* data, std::size_t size, ImageRange range)
	{
		const auto address = reinterpret_cast<std::uintptr_t>(data);
		const auto begin = reinterpret_cast<std::uintptr_t>(range.data);
		const auto end = begin + range.size;

		return address >= begin and address <= end and size <= end - address;
	}

	ImageRange GetImage()
	{
		const auto image = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));

		if (!image)
		{
			return {};
		}

		const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);

		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return {};
		}

		const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);

		if (nt->Signature != IMAGE_NT_SIGNATURE)
		{
			return {};
		}

		return { image, nt->OptionalHeader.SizeOfImage };
	}

	ImageRange GetText(ImageRange image)
	{
		if (!image.data)
		{
			return {};
		}

		const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data);
		const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data + dos->e_lfanew);
		const auto sections = IMAGE_FIRST_SECTION(nt);

		for (std::uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index)
		{
			const auto& section = sections[index];

			if (std::memcmp(section.Name, ".text", 5) == 0)
			{
				return { image.data + section.VirtualAddress, section.Misc.VirtualSize };
			}
		}

		return {};
	}

	bool ReadGameBuild(unsigned& gameVersion, unsigned& gameBuild)
	{
		const auto image = GetImage();
		const auto text = GetText(image);

		if (!text.data or text.size < 23)
		{
			return false;
		}

		const std::uint8_t* result = nullptr;

		for (std::size_t offset = 0; offset + 23 <= text.size; ++offset)
		{
			const auto code = text.data + offset;

			if (!Match(code, { 0xC7, 0x05 })
				or !Match(code + 10, { 0xC7, 0x05 })
				or !Match(code + 20, { 0x48, 0x8B, 0x05 })
			)
			{
				continue;
			}

			const auto versionTarget = GetTarget(code + 10, ReadDisp(code + 2));
			const auto buildTarget = GetTarget(code + 20, ReadDisp(code + 12));
			const auto versionAddress = reinterpret_cast<std::uintptr_t>(versionTarget);
			const auto buildAddress = reinterpret_cast<std::uintptr_t>(buildTarget);

			if (buildAddress != versionAddress + sizeof(std::uint32_t)
				or !HasRange(versionTarget, sizeof(std::uint32_t) * 2, image)
			)
			{
				continue;
			}

			if (result)
			{
				return false;
			}

			result = code;
		}

		if (!result)
		{
			return false;
		}

		std::memcpy(&gameVersion, result + 6, sizeof(gameVersion));
		std::memcpy(&gameBuild, result + 16, sizeof(gameBuild));

		return true;
	}

	bool CheckGameCompatibility()
	{
		unsigned gameVersion = 0;
		unsigned gameBuild = 0;

		if (!ReadGameBuild(gameVersion, gameBuild))
		{
			WriteLog("Compatibility", "X4 version marker unavailable; native initialization aborted before hooks");

			return false;
		}

		std::ostringstream stream;
		stream << "X4 " << gameVersion / 100 << '.'
			<< std::setfill('0') << std::setw(2) << gameVersion % 100
			<< " build " << gameBuild;

		if (gameVersion != supportedGameVersion or gameBuild != supportedGameBuild)
		{
			stream << " unsupported; expected " << supportedGameVersion / 100 << '.'
				<< std::setw(2) << supportedGameVersion % 100
				<< " build " << supportedGameBuild;
			WriteLog("Compatibility", stream.str());

			return false;
		}

		stream << " accepted";
		WriteLog("Compatibility", stream.str());

		return true;
	}

	std::uint8_t* FindCode(ImageRange text, std::initializer_list<std::uint8_t> bytes)
	{
		if (!text.data or bytes.size() > text.size)
		{
			return nullptr;
		}

		std::uint8_t* result = nullptr;

		for (std::size_t offset = 0; offset + bytes.size() <= text.size; ++offset)
		{
			const auto code = text.data + offset;

			if (!Match(code, bytes))
			{
				continue;
			}

			if (result)
			{
				return nullptr;
			}

			result = code;
		}

		return result;
	}

	MoveCalc FindMoveCalc()
	{
		const auto text = GetText(GetImage());

		if (!text.data or text.size < 57)
		{
			return nullptr;
		}

		std::uint8_t* result = nullptr;

		for (std::size_t offset = 0; offset + 57 <= text.size; ++offset)
		{
			const auto code = text.data + offset;

			if (!Match(code, {
				0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48,
				0x89, 0x68, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48,
				0x81, 0xEC, 0x80, 0x00, 0x00, 0x00
			})
				or !Match(code + 22, {
					0x48, 0x8B, 0x1D
				})
				or !Match(code + 29, {
					0x49, 0x8B, 0xF9, 0x0F, 0x29, 0x70, 0xD8, 0x49,
					0x8B, 0xF0, 0x0F, 0x29, 0x78, 0xC8, 0x48, 0x8B,
					0xEA, 0xF3, 0x0F, 0x10, 0xBA, 0x04, 0x09, 0x00,
					0x00, 0x4C, 0x8B, 0xF1
				})
			)
			{
				continue;
			}

			if (result)
			{
				return nullptr;
			}

			result = code;
		}

		return reinterpret_cast<MoveCalc>(result);
	}

	TheoreticalMoveCalc FindTheoreticalMoveCalc()
	{
		const auto text = GetText(GetImage());

		if (!text.data or text.size < 42)
		{
			return nullptr;
		}

		std::uint8_t* result = nullptr;

		for (std::size_t offset = 0; offset + 42 <= text.size; ++offset)
		{
			const auto code = text.data + offset;

			if (!Match(code, {
				0x48, 0x83, 0xEC, 0x38, 0xF3, 0x0F, 0x10, 0x1D
			})
				or !Match(code + 12, {
					0x0F, 0x29, 0x74, 0x24, 0x20, 0x4D, 0x85, 0xC9,
					0x74, 0x0E, 0xF3, 0x41, 0x0F, 0x10, 0x69, 0x18,
					0xF3, 0x41, 0x0F, 0x10, 0x61, 0x1C, 0xEB, 0x06,
					0x0F, 0x28, 0xEB, 0x0F, 0x28, 0xE3
				})
			)
			{
				continue;
			}

			if (result)
			{
				return nullptr;
			}

			result = code;
		}

		return reinterpret_cast<TheoreticalMoveCalc>(result);
	}

	MoveApply FindMoveApply()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x48,
			0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x48, 0x8D,
			0x68, 0xD8, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00,
			0x00
		});

		return reinterpret_cast<MoveApply>(code);
	}

	ShieldAdd FindShieldAdd()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
			0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
			0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x0F,
			0x29, 0x74, 0x24, 0x20, 0x48, 0x8B, 0x11
		});

		return reinterpret_cast<ShieldAdd>(code);
	}

	ShieldSet FindShieldSet()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29,
			0x74, 0x24, 0x40, 0x48, 0x8B, 0xD9, 0x48, 0x8B,
			0x49, 0x20, 0x0F, 0x28, 0xF1, 0x0F, 0x29, 0x7C,
			0x24, 0x30, 0x44, 0x0F, 0x29, 0x44, 0x24, 0x20,
			0x44, 0x0F, 0x28, 0xC2
		});

		return reinterpret_cast<ShieldSet>(code);
	}

	WeaponReload FindWeaponReload()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
			0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x90, 0x98, 0x20,
			0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x88,
			0x00, 0x00, 0x00
		});

		return reinterpret_cast<WeaponReload>(code);
	}

	ContainerBuildResourceCollector FindContainerBuildResourceCollector()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54,
			0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55,
			0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
			0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE9, 0x48,
			0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x4D, 0x8B
		});

		return reinterpret_cast<ContainerBuildResourceCollector>(code);
	}

	EquipmentBuildResourceCollector FindEquipmentBuildResourceCollector()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
			0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
			0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xF0,
			0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xC8, 0x0F,
			0x29, 0x78, 0xB8, 0x44, 0x0F, 0x29, 0x40, 0xA8,
			0x4C
		});

		return reinterpret_cast<EquipmentBuildResourceCollector>(code);
	}

	MapInfoEligibility FindMapInfoEligibility()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
			0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C,
			0x89, 0x74, 0x24, 0x20, 0x55, 0x48, 0x8B, 0xEC,
			0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x44,
			0x0F, 0xB6, 0xC2
		});

		return reinterpret_cast<MapInfoEligibility>(code);
	}

	std::uint8_t* FindMapInfoEligibilityReturn(MapInfoEligibility eligibility)
	{
		const auto text = GetText(GetImage());

		if (!text.data or !eligibility or text.size < 32)
		{
			return nullptr;
		}

		std::uint8_t* result = nullptr;

		for (std::size_t offset = 0; offset + 25 <= text.size; ++offset)
		{
			const auto code = text.data + offset;

			if (!Match(code, {
				0xC6, 0x44, 0x24, 0x20, 0x01, 0x45, 0x33, 0xC9,
				0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xCB,
				0xE8
			}))
			{
				continue;
			}

			const auto next = code + 21;
			const auto target = GetTarget(next, ReadDisp(code + 17));

			if (target != reinterpret_cast<std::uint8_t*>(eligibility) or !Match(next, {
				0x84, 0xC0, 0x74, 0x1E
			}))
			{
				continue;
			}

			if (result)
			{
				return nullptr;
			}

			result = next;
		}

		return result;
	}

	std::uint8_t* FindTradeFilterEligibilityReturn(MapInfoEligibility eligibility)
	{
		const auto text = GetText(GetImage());

		if (!text.data or !eligibility or text.size < 48)
		{
			return nullptr;
		}

		const auto filter = FindCode(text, {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
			0xEC, 0x40, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9,
			0x48, 0x85, 0xD2, 0x74, 0x5B, 0x48, 0x8B, 0x02,
			0x48, 0x8B, 0xCB, 0xBA, 0x6F, 0x00, 0x00, 0x00,
			0xFF, 0x90, 0xE8, 0x11, 0x00, 0x00, 0x84, 0xC0,
			0x74
		});

		if (!filter or !HasRange(filter, 0x90, text))
		{
			return nullptr;
		}

		std::uint8_t* result = nullptr;
		const auto target = reinterpret_cast<std::uint8_t*>(eligibility);

		for (std::size_t offset = 0; offset + 5 <= 0x90; ++offset)
		{
			const auto code = filter + offset;

			if (*code != 0xE8)
			{
				continue;
			}

			const auto next = code + 5;

			if (GetTarget(next, ReadDisp(code + 1)) != target)
			{
				continue;
			}

			if (result)
			{
				return nullptr;
			}

			result = next;
		}

		return result;
	}

	MapInfoBuilder FindMapInfoBuilder()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x4C,
			0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18, 0x48,
			0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41,
			0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
			0x8D, 0xAC, 0x24, 0x70, 0xCF, 0xFF, 0xFF, 0xB8,
			0xB0, 0x31, 0x00, 0x00
		});

		return reinterpret_cast<MapInfoBuilder>(code);
	}

	TradeAccountHolder FindTradeAccountHolder()
	{
		const auto text = GetText(GetImage());

		const auto code = FindCode(text, {
			0x40, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x68, 0x48,
			0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x45, 0x84, 0xC0,
			0x74, 0x16, 0x80, 0xB9, 0x08, 0x07, 0x00, 0x00,
			0x00, 0x74, 0x0D, 0x48, 0x89, 0x0A, 0x48, 0x8B,
			0xC2, 0x48, 0x83, 0xC4, 0x68, 0x5F, 0x5E, 0xC3
		});

		return reinterpret_cast<TradeAccountHolder>(code);
	}

	std::uint8_t* FindTradeBudgetReturn(TradeAccountHolder account)
	{
		if (!account)
		{
			return nullptr;
		}

		const auto text = GetText(GetImage());

		if (!text.data or text.size < 48)
		{
			return nullptr;
		}

		std::uint8_t* result = nullptr;
		const auto target = reinterpret_cast<std::uint8_t*>(account);

		for (std::size_t offset = 0; offset + 44 <= text.size; ++offset)
		{
			const auto code = text.data + offset;

			if (!Match(code, {
				0x49, 0x8B, 0x07, 0xBA, 0x75, 0x00, 0x00, 0x00,
				0x49, 0x8B, 0xCF, 0xFF, 0x90, 0xE8, 0x11, 0x00,
				0x00, 0x49, 0x8B, 0xCF, 0x84, 0xC0, 0x74, 0x40,
				0x45, 0x33, 0xC0, 0x48, 0x8D, 0x55, 0x98, 0xE8
			})
				or !Match(code + 36, {
					0x48, 0x8B, 0x08, 0x48, 0x85, 0xC9, 0x75, 0x2C
				})
			)
			{
				continue;
			}

			const auto next = code + 36;
			const auto call = GetTarget(next, ReadDisp(code + 32));

			if (call != target)
			{
				continue;
			}

			if (result)
			{
				return nullptr;
			}

			result = next;
		}

		return result;
	}

	bool FindResolver()
	{
		const auto image = GetImage();
		const auto text = GetText(image);
		const auto getter = reinterpret_cast<std::uint8_t*>(GetProcAddress(GetModuleHandleW(nullptr), speedName.data()));

		if (!getter or !HasRange(getter, 96, text))
		{
			return false;
		}

		ResolveObj result = nullptr;
		void** root = nullptr;

		for (std::size_t offset = 0; offset + 21 <= 96; ++offset)
		{
			const auto code = getter + offset;

			if (!Match(code, {
				0x41, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8B,
				0x0D
			})
				or !Match(code + 13, {
					0x4C, 0x8B, 0xF2, 0xE8
				})
			)
			{
				continue;
			}

			const auto target = GetTarget(code + 21, ReadDisp(code + 17));
			const auto source = GetTarget(code + 13, ReadDisp(code + 9));

			if (!HasRange(target, 12, text) or !HasRange(source, sizeof(void*), image)
				or !Match(target, {
					0x48, 0x89, 0x5C, 0x24, 0x08, 0x45, 0x8B, 0xD8,
					0x4C, 0x8B, 0xCA
				})
			)
			{
				continue;
			}

			if (result)
			{
				return false;
			}

			result = reinterpret_cast<ResolveObj>(target);
			root = reinterpret_cast<void**>(source);
		}

		resolveObj = result;
		universe = root;

		return resolveObj and universe;
	}

	void* ResolveObject(std::uint64_t object)
	{
		if (!resolveObj or !universe or !*universe)
		{
			return nullptr;
		}

		return resolveObj(*universe, object, 4);
	}

	bool ReadComponentId(void* object, std::uint64_t& id)
	{
		if (!object)
		{
			return false;
		}

		std::memcpy(&id, static_cast<std::uint8_t*>(object) + componentId, sizeof(id));

		return id != 0;
	}

	bool ReadComponentMacro(void* object, std::string_view& macro)
	{
		if (!object)
		{
			return false;
		}

		const auto definition = static_cast<std::uint8_t*>(object) + componentDefinition;
		void** table = nullptr;
		std::memcpy(&table, definition, sizeof(table));

		if (!table or !table[4])
		{
			return false;
		}

		using GetMacroName = void* (__fastcall*)(void*);
		const auto getMacroName = reinterpret_cast<GetMacroName>(table[4]);
		const auto text = static_cast<std::uint64_t*>(getMacroName(definition));

		if (!text)
		{
			return false;
		}

		const auto length = static_cast<std::size_t>(text[2]);
		const auto capacity = static_cast<std::size_t>(text[3]);

		if (length == 0 or length > 256 or capacity < length)
		{
			return false;
		}

		const auto data = capacity < 16
			? reinterpret_cast<const char*>(text)
			: reinterpret_cast<const char*>(text[0]);

		if (!data)
		{
			return false;
		}

		macro = { data, length };

		return true;
	}

	bool IsNomadShip(void* object)
	{
		std::string_view macro;

		return ReadComponentMacro(object, macro) and macro == nomadMacro;
	}

	bool IsNomadShip(void* object, std::uint64_t& ship)
	{
		return IsNomadShip(object) and ReadComponentId(object, ship);
	}

	bool GetEnginePower(std::uint64_t ship, float& value)
	{
		std::shared_lock guard(powerLock);
		const auto entry = enginePower.find(ship);

		if (entry == enginePower.end())
		{
			return false;
		}

		value = entry->second;

		return true;
	}

	bool GetWeaponGroupPower(void* weapon, float& value)
	{
		void* group = nullptr;
		std::memcpy(&group, static_cast<std::uint8_t*>(weapon) + shieldGroup, sizeof(group));

		if (!group)
		{
			return false;
		}

		std::shared_lock guard(powerLock);
		const auto entry = groupPower.find(group);

		if (entry == groupPower.end())
		{
			return false;
		}

		value = entry->second.weapons;

		return true;
	}

	bool GetShieldGroupPower(void* group, float& value)
	{
		std::shared_lock guard(powerLock);
		const auto entry = groupPower.find(group);

		if (entry == groupPower.end())
		{
			return false;
		}

		value = entry->second.shields;

		return true;
	}

	std::vector<void*> GetShipGroups(void* ship)
	{
		std::vector<void*> groups;
		void** begin = nullptr;
		void** end = nullptr;
		const auto data = static_cast<std::uint8_t*>(ship);
		std::memcpy(&begin, data + shipGroupsBegin, sizeof(begin));
		std::memcpy(&end, data + shipGroupsEnd, sizeof(end));

		if (!begin or !end)
		{
			return groups;
		}

		const auto first = reinterpret_cast<std::uintptr_t>(begin);
		const auto last = reinterpret_cast<std::uintptr_t>(end);

		if (last < first or (last - first) % sizeof(void*) != 0)
		{
			return groups;
		}

		const auto count = (last - first) / sizeof(void*);

		if (count > 4096)
		{
			return groups;
		}

		groups.reserve(count);

		for (auto current = begin; current != end; ++current)
		{
			if (*current)
			{
				groups.push_back(*current);
			}
		}

		return groups;
	}

	void ClearPower()
	{
		std::unique_lock guard(powerLock);
		enginePower.clear();
		groupPower.clear();
		shipGroups.clear();
	}

	void RemovePower(std::uint64_t ship)
	{
		std::unique_lock guard(powerLock);
		enginePower.erase(ship);
		const auto groups = shipGroups.find(ship);

		if (groups != shipGroups.end())
		{
			for (const auto group : groups->second)
			{
				groupPower.erase(group);
			}

			shipGroups.erase(groups);
		}
	}

	bool GetMoveContext(double& context)
	{
		const auto slots = reinterpret_cast<void**>(__readgsqword(0x58));

		if (!slots or !slots[0])
		{
			return false;
		}

		const auto local = static_cast<std::uint8_t*>(slots[0]);
		std::memcpy(&context, local + moveContext, sizeof(context));

		return true;
	}

	bool ScaleForwardThrust(const void* thrust, float factor, std::array<float, 9>& local)
	{
		if (!thrust or factor == 1.0F)
		{
			return false;
		}

		std::memcpy(local.data(), thrust, sizeof(local));
		local[0] *= factor;
		local[1] *= factor;

		return std::isfinite(local[0]) and std::isfinite(local[1]);
	}

	void __fastcall MoveCalcHook(void* ship, void* macro, const void* thrust, void* stats) noexcept
	{
		std::array<float, 9> local{};
		const void* effective = thrust;

		try
		{
			std::uint64_t id = 0;
			float factor = 1.0F;

			if (ReadComponentId(ship, id) and GetEnginePower(id, factor) and ScaleForwardThrust(thrust, factor, local))
			{
				effective = local.data();
			}
		}
		catch (...)
		{
		}

		moveCall(ship, macro, effective, stats);
	}

	SpeedInfo* __fastcall GetDefensibleSpeedsHook(SpeedInfo* result, std::uint64_t ship) noexcept
	{
		const auto previousFactor = speedQueryFactor;
		float factor = 1.0F;

		try
		{
			GetEnginePower(ship, factor);
		}
		catch (...)
		{
			factor = 1.0F;
		}

		speedQueryFactor = factor;
		const auto response = getDefensibleSpeedsCall(result, ship);
		speedQueryFactor = previousFactor;

		return response;
	}

	void __fastcall TheoreticalMoveHook(void* macro, const void* thrust, void* stats, void* shipMod, void* engineMod) noexcept
	{
		std::array<float, 9> local{};
		const void* effective = thrust;

		try
		{
			if (ScaleForwardThrust(thrust, speedQueryFactor, local))
			{
				effective = local.data();
			}
		}
		catch (...)
		{
		}

		theoreticalMoveCall(macro, effective, stats, shipMod, engineMod);
	}

	bool ApplyMovement(void* ship)
	{
		double context;

		if (!GetMoveContext(context))
		{
			return false;
		}

		alignas(16) std::array<std::uint8_t, moveSize> stats{};
		applyMove(ship, stats.data(), 0, context, 0, nullptr);

		return true;
	}

	bool ReadShieldCap(void* shield, double& capacity)
	{
		if (!shield)
		{
			return false;
		}

		const auto data = static_cast<std::uint8_t*>(shield);
		std::memcpy(&capacity, data + shieldCapacity, sizeof(capacity));

		return std::isfinite(capacity) and capacity >= 0.0;
	}

	bool ReadShield(void* shield, double context, double& value)
	{
		const auto data = static_cast<std::uint8_t*>(shield);
		void* link;
		std::memcpy(&link, data + shieldLink, sizeof(link));

		if (!link)
		{
			return false;
		}

		std::int32_t offset;
		std::memcpy(&offset, static_cast<std::uint8_t*>(link) + 4, sizeof(offset));
		const auto object = data + shieldLink + offset;
		void** table;
		std::memcpy(&table, object, sizeof(table));

		if (!table or !table[5])
		{
			return false;
		}

		const auto read = reinterpret_cast<ShieldRead>(table[5]);
		value = read(object, context);

		return std::isfinite(value) and value >= 0.0;
	}

	bool ScaleShield(void* shield, float before, float after)
	{
		double current;

		if (!ReadShieldCap(shield, current) or before <= 0.0F)
		{
			return false;
		}

		const auto ratio = static_cast<double>(after) / before;
		const auto next = current * ratio;

		if (!std::isfinite(next) or next < 0.0)
		{
			return false;
		}

		double context = 0.0;
		double charge = 0.0;
		const auto hasCharge = GetMoveContext(context) and ReadShield(shield, context, charge);
		const auto data = static_cast<std::uint8_t*>(shield);
		std::memcpy(data + shieldCapacity, &next, sizeof(next));

		if (hasCharge)
		{
			const auto nextCharge = charge * ratio;

			if (std::isfinite(nextCharge) and nextCharge >= 0.0)
			{
				setShield(data + shieldState, nextCharge, context);
			}
		}

		return true;
	}

	void SetGroupPower(std::uint64_t ship, const std::vector<void*>& groups, float weapons, float shields)
	{
		std::vector<ShieldScale> scales;

		{
			std::unique_lock guard(powerLock);
			const auto previous = shipGroups.find(ship);

			if (previous != shipGroups.end())
			{
				for (const auto group : previous->second)
				{
					if (std::find(groups.begin(), groups.end(), group) == groups.end())
					{
						groupPower.erase(group);
					}
				}
			}

			scales.reserve(groups.size());

			for (const auto group : groups)
			{
				const auto current = groupPower.find(group);
				const auto previousShield = current == groupPower.end() ? 1.0F : current->second.shields;
				groupPower[group] = { weapons, shields };

				if (previousShield != shields)
				{
					scales.push_back({ group, previousShield });
				}
			}

			shipGroups[ship] = groups;
		}

		for (const auto& scale : scales)
		{
			ScaleShield(scale.group, scale.previous, shields);
		}
	}

	void __fastcall ShieldHook(void* group, void* generator) noexcept
	{
		double before = 0.0;
		ReadShieldCap(group, before);
		shieldCall(group, generator);

		try
		{
			float factor;
			double after;

			if (!GetShieldGroupPower(group, factor) or factor == 1.0F or !ReadShieldCap(group, after))
			{
				return;
			}

			const auto capacity = before + ((after - before) * factor);

			if (!std::isfinite(capacity) or capacity < 0.0)
			{
				return;
			}

			const auto data = static_cast<std::uint8_t*>(group);
			std::memcpy(data + shieldCapacity, &capacity, sizeof(capacity));
		}
		catch (...)
		{
		}
	}

	float __fastcall WeaponReloadHook(void* weapon) noexcept
	{
		auto rate = weaponReloadCall(weapon);

		try
		{
			float factor;

			if (!GetWeaponGroupPower(weapon, factor) or factor == 1.0F)
			{
				return rate;
			}

			const auto next = rate * factor;

			if (!std::isfinite(next) or next < 0.0F)
			{
				return rate;
			}

			rate = next;
		}
		catch (...)
		{
		}

		return rate;
	}

	bool HasTradeFilterEntries(void* object)
	{
		if (!object)
		{
			return false;
		}

		const auto hasEntries = [object](std::size_t offset)
		{
			void* begin = nullptr;
			void* end = nullptr;

			std::memcpy(&begin, static_cast<std::uint8_t*>(object) + offset, sizeof(begin));
			std::memcpy(&end, static_cast<std::uint8_t*>(object) + offset + sizeof(void*), sizeof(end));

			return begin and end and begin != end;
		};

		return hasEntries(tradeFilterEntriesPrimary) or hasEntries(tradeFilterEntriesSecondary);
	}

	bool GetMapInfoState(std::uint64_t ship, bool& visible)
	{
		std::lock_guard guard(mapInfoLock);
		const auto entry = mapInfoVisible.find(ship);

		if (entry == mapInfoVisible.end())
		{
			return false;
		}

		visible = entry->second;

		return true;
	}

	bool GetSelectedMapInfoState(void* records, std::uint64_t& ship, bool& visible)
	{
		if (!records)
		{
			return false;
		}

		void* begin = nullptr;
		void* end = nullptr;
		const auto wrapper = static_cast<std::uint8_t*>(records);
		std::memcpy(&begin, wrapper, sizeof(begin));
		std::memcpy(&end, wrapper + sizeof(void*), sizeof(end));

		if (!begin or !end or begin == end)
		{
			return false;
		}

		void* object = nullptr;
		std::memcpy(&object, begin, sizeof(object));

		return object and IsNomadShip(object, ship) and GetMapInfoState(ship, visible);
	}

	void SetMapInfoVisible(std::uint64_t ship, bool visible)
	{
		std::lock_guard guard(mapInfoLock);
		mapInfoVisible[ship] = visible;
	}

	void RemoveMapInfoState(std::uint64_t ship)
	{
		std::lock_guard guard(mapInfoLock);
		mapInfoVisible.erase(ship);
	}

	bool __fastcall MapInfoEligibilityHook(void* object, std::uintptr_t arg2, std::uintptr_t arg3, std::uintptr_t arg4, std::uintptr_t arg5, void* arg6, void* arg7) noexcept
	{
		const auto original = mapInfoEligibilityCall(object, arg2, arg3, arg4, arg5, arg6, arg7);

		try
		{
			const auto caller = reinterpret_cast<std::uint8_t*>(_ReturnAddress());

			if ((caller != mapInfoEligibilityReturn and caller != tradeFilterEligibilityReturn) or !object)
			{
				return original;
			}

			std::uint64_t ship = 0;

			if (!IsNomadShip(object, ship))
			{
				return original;
			}

			bool visible = false;

			if (!GetMapInfoState(ship, visible))
			{
				return original;
			}

			if (caller == mapInfoEligibilityReturn)
			{
				return original or visible;
			}

			const auto trade = mapTradeEnabled.load(std::memory_order_relaxed);
			const auto entries = trade and HasTradeFilterEntries(object);

			return trade and (original or entries);
		}
		catch (...)
		{
			WriteLog("Map Info", "eligibility hook failed");

			return original;
		}
	}

	void __fastcall MapInfoHook(void* map, void* records, bool general, void* arg4, void* arg5, void* arg6) noexcept
	{
		if (!general)
		{
			mapInfoCall(map, records, false, arg4, arg5, arg6);

			return;
		}

		bool effective = true;
		bool suppressed = false;

		try
		{
			std::uint64_t ship = 0;
			bool visible = false;

			if (GetSelectedMapInfoState(records, ship, visible))
			{
				const auto trade = mapTradeEnabled.load(std::memory_order_relaxed);

				if (!visible and !trade)
				{
					suppressed = true;
				}
				else
				{
					effective = visible;
				}
			}
		}
		catch (...)
		{
			WriteLog("Map Info", "builder hook failed");
			suppressed = false;
			effective = true;
		}

		if (!suppressed)
		{
			mapInfoCall(map, records, effective, arg4, arg5, arg6);
		}
	}

	bool HasOwnAccount(void* object)
	{
		if (!object)
		{
			return false;
		}

		std::uint8_t state = 0;
		std::memcpy(&state, static_cast<std::uint8_t*>(object) + ownAccountState, sizeof(state));

		return state != 0;
	}

	void* __fastcall TradeAccountHook(void* container, void* result, bool allowOwnAccount) noexcept
	{
		const auto caller = reinterpret_cast<std::uint8_t*>(_ReturnAddress());
		auto response = tradeAccountCall(container, result, allowOwnAccount);

		try
		{
			if (caller != tradeBudgetReturn or allowOwnAccount or !result)
			{
				return response;
			}

			if (!IsNomadShip(container) or !HasOwnAccount(container))
			{
				return response;
			}

			std::memcpy(result, &container, sizeof(container));

			return result;
		}
		catch (...)
		{
			WriteLog("Trade Account", "hook failed");

			return response;
		}
	}

	bool __fastcall ContainerIsStationHook(void* container) noexcept
	{
		if (container and container == nomadBuildResourceContainer)
		{
			return true;
		}

		return containerIsStationCall(container);
	}

	bool EnsureContainerIsStationSlot(void* container)
	{
		if (!container)
		{
			return false;
		}

		auto vtable = *reinterpret_cast<void***>(container);

		if (!vtable)
		{
			return false;
		}

		auto slot = reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(vtable) + 0x2330);
		std::lock_guard guard(buildResourceLock);

		if (containerIsStationSlot)
		{
			return containerIsStationSlot == slot;
		}

		void* original = nullptr;
		std::memcpy(&original, slot, sizeof(original));
		const auto text = GetText(GetImage());

		if (!original or !HasRange(reinterpret_cast<std::uint8_t*>(original), 3, text)
			or !Match(reinterpret_cast<std::uint8_t*>(original), {
				0x32, 0xC0, 0xC3
			})
		)
		{
			return false;
		}

		DWORD protection = 0;

		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
		{
			return false;
		}

		containerIsStationCall = reinterpret_cast<ContainerIsStation>(original);
		*slot = reinterpret_cast<void*>(ContainerIsStationHook);

		DWORD restored = 0;
		VirtualProtect(slot, sizeof(void*), protection, &restored);
		containerIsStationSlot = slot;
		WriteLog("Build Resources", "station gate armed");

		return true;
	}

	void __fastcall ContainerBuildResourceHook(void* container, void* resources, void* arg3, void* arg4, void* arg5, bool reset) noexcept
	{
		const auto previous = nomadBuildResourceContainer;

		try
		{
			if (IsNomadShip(container) and EnsureContainerIsStationSlot(container))
			{
				nomadBuildResourceContainer = container;
			}
		}
		catch (...)
		{
			nomadBuildResourceContainer = previous;
		}

		containerBuildResourceCall(container, resources, arg3, arg4, arg5, reset);
		nomadBuildResourceContainer = previous;
	}

	bool __fastcall ContainerTypeCheckHook(void* container, std::uint32_t type) noexcept
	{
		if (container and container == nomadEquipmentResourceContainer)
		{
			if (type == 0x75)
			{
				return false;
			}

			if (type == 0x62)
			{
				return true;
			}
		}

		return containerTypeCheckCall(container, type);
	}

	bool EnsureContainerTypeCheckSlot(void* container)
	{
		if (!container)
		{
			return false;
		}

		auto vtable = *reinterpret_cast<void***>(container);

		if (!vtable)
		{
			return false;
		}

		auto slot = reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(vtable) + 0x11E8);
		std::lock_guard guard(buildResourceLock);

		if (containerTypeCheckSlot)
		{
			return containerTypeCheckSlot == slot;
		}

		void* original = nullptr;
		std::memcpy(&original, slot, sizeof(original));
		const auto text = GetText(GetImage());

		if (!original or !HasRange(reinterpret_cast<std::uint8_t*>(original), 16, text))
		{
			return false;
		}

		const auto check = reinterpret_cast<ContainerTypeCheck>(original);

		if (!check(container, 0x75) or check(container, 0x62))
		{
			return false;
		}

		DWORD protection = 0;

		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
		{
			return false;
		}

		containerTypeCheckCall = check;
		*slot = reinterpret_cast<void*>(ContainerTypeCheckHook);

		DWORD restored = 0;
		VirtualProtect(slot, sizeof(void*), protection, &restored);
		containerTypeCheckSlot = slot;
		WriteLog("Build Resources", "equipment type gate armed");

		return true;
	}

	void __fastcall EquipmentBuildResourceHook(void* container) noexcept
	{
		const auto previous = nomadEquipmentResourceContainer;

		try
		{
			if (IsNomadShip(container) and EnsureContainerTypeCheckSlot(container))
			{
				nomadEquipmentResourceContainer = container;
			}
		}
		catch (...)
		{
			nomadEquipmentResourceContainer = previous;
		}

		equipmentBuildResourceCall(container);
		nomadEquipmentResourceContainer = previous;
	}

	bool GetLuaText(LuaState* state, int index, std::string_view& value)
	{
		std::size_t length;
		const auto text = luaText(state, index, &length);

		if (!text)
		{
			return false;
		}

		value = { text, length };

		return true;
	}

	bool ParseShip(std::string_view text, std::uint64_t& ship)
	{
		if (text.ends_with("ULL"))
		{
			text.remove_suffix(3);
		}
		else if (text.ends_with("LL"))
		{
			text.remove_suffix(2);
		}

		int base = 10;

		if (text.starts_with("0x") or text.starts_with("0X"))
		{
			text.remove_prefix(2);
			base = 16;
		}

		const auto result = std::from_chars(text.data(), text.data() + text.size(), ship, base);

		return !text.empty() and result.ec == std::errc() and result.ptr == text.data() + text.size() and ship != 0;
	}

	int PushLuaResult(LuaState* state, bool value)
	{
		luaPushBool(state, value ? 1 : 0);

		return 1;
	}

	bool GetLuaShip(LuaState* state, int index, std::uint64_t& ship)
	{
		std::string_view value;

		return GetLuaText(state, index, value) and ParseShip(value, ship);
	}

	int LuaSetPower(LuaState* state)
	{
		if (luaTop(state) < 4 or !luaIsNumber(state, 2) or !luaIsNumber(state, 3) or !luaIsNumber(state, 4))
		{
			return PushLuaResult(state, false);
		}

		std::uint64_t ship = 0;
		const auto weapons = static_cast<float>(luaNumber(state, 2));
		const auto shields = static_cast<float>(luaNumber(state, 3));
		const auto engines = static_cast<float>(luaNumber(state, 4));

		if (!GetLuaShip(state, 1, ship) or !IsPower(weapons) or !IsPower(shields) or !IsPower(engines))
		{
			return PushLuaResult(state, false);
		}

		return PushLuaResult(state, SetPower(ship, weapons, shields, engines));
	}

	int LuaClearPower(LuaState* state)
	{
		ClearPower();

		return PushLuaResult(state, true);
	}

	int LuaRemovePower(LuaState* state)
	{
		std::uint64_t ship = 0;

		if (!GetLuaShip(state, 1, ship))
		{
			return PushLuaResult(state, false);
		}

		RemovePower(ship);

		return PushLuaResult(state, true);
	}

	int LuaResetPower(LuaState* state)
	{
		std::uint64_t ship = 0;

		if (!GetLuaShip(state, 1, ship))
		{
			return PushLuaResult(state, false);
		}

		const auto reset = SetPower(ship, 1.0F, 1.0F, 1.0F);
		RemovePower(ship);

		return PushLuaResult(state, reset);
	}

	int LuaSetMapInfoVisible(LuaState* state)
	{
		std::uint64_t ship = 0;

		if (luaTop(state) < 2 or !GetLuaShip(state, 1, ship))
		{
			return PushLuaResult(state, false);
		}

		SetMapInfoVisible(ship, luaToBool(state, 2) != 0);

		return PushLuaResult(state, true);
	}

	int LuaRemoveMapInfoState(LuaState* state)
	{
		std::uint64_t ship = 0;

		if (!GetLuaShip(state, 1, ship))
		{
			return PushLuaResult(state, false);
		}

		RemoveMapInfoState(ship);

		return PushLuaResult(state, true);
	}

	int LuaSetMapTradeEnabled(LuaState* state)
	{
		if (luaTop(state) < 1)
		{
			return PushLuaResult(state, false);
		}

		mapTradeEnabled.store(luaToBool(state, 1) != 0, std::memory_order_relaxed);

		return PushLuaResult(state, true);
	}

	void SetLuaFunction(LuaState* state, const char* name, LuaHandler function)
	{
		luaPushCClosure(state, function, 0);
		luaSetField(state, -2, name);
	}

	void PushLuaModule(LuaState* state)
	{
		luaCreateTable(state, 0, 7);
		SetLuaFunction(state, "setPower", LuaSetPower);
		SetLuaFunction(state, "clearPower", LuaClearPower);
		SetLuaFunction(state, "removePower", LuaRemovePower);
		SetLuaFunction(state, "resetPower", LuaResetPower);
		SetLuaFunction(state, "setMapInfoVisible", LuaSetMapInfoVisible);
		SetLuaFunction(state, "removeMapInfoState", LuaRemoveMapInfoState);
		SetLuaFunction(state, "setMapTradeEnabled", LuaSetMapTradeEnabled);
	}

	bool InitHooks()
	{
		const auto game = GetModuleHandleW(nullptr);
		const auto moveCalc = FindMoveCalc();
		const auto theoreticalMove = FindTheoreticalMoveCalc();
		const auto getDefensibleSpeeds = game ? reinterpret_cast<GetDefensibleSpeeds>(GetProcAddress(game, speedName.data())) : nullptr;
		applyMove = FindMoveApply();

		const auto addShield = FindShieldAdd();
		setShield = FindShieldSet();
		const auto weaponReload = FindWeaponReload();

		const auto containerBuildResources = FindContainerBuildResourceCollector();
		const auto equipmentBuildResources = FindEquipmentBuildResourceCollector();

		const auto tradeAccount = FindTradeAccountHolder();
		tradeBudgetReturn = FindTradeBudgetReturn(tradeAccount);

		const auto mapInfoBuilder = FindMapInfoBuilder();
		const auto mapInfoEligibility = FindMapInfoEligibility();
		mapInfoEligibilityReturn = FindMapInfoEligibilityReturn(mapInfoEligibility);
		tradeFilterEligibilityReturn = FindTradeFilterEligibilityReturn(mapInfoEligibility);

		const auto resolver = FindResolver();
		bool valid = true;

		CheckBind("lua_tolstring", luaText != nullptr, valid);
		CheckBind("lua_gettop", luaTop != nullptr, valid);
		CheckBind("lua_tonumber", luaNumber != nullptr, valid);
		CheckBind("lua_isnumber", luaIsNumber != nullptr, valid);
		CheckBind("lua_toboolean", luaToBool != nullptr, valid);
		CheckBind("lua_pushcclosure", luaPushCClosure != nullptr, valid);
		CheckBind("lua_createtable", luaCreateTable != nullptr, valid);
		CheckBind("lua_setfield", luaSetField != nullptr, valid);
		CheckBind("MoveCalc", moveCalc != nullptr, valid);
		CheckBind("TheoreticalMoveCalc", theoreticalMove != nullptr, valid);
		CheckBind("GetDefensibleSpeeds", getDefensibleSpeeds != nullptr, valid);
		CheckBind("MoveApply", applyMove != nullptr, valid);
		CheckBind("AddShield", addShield != nullptr, valid);
		CheckBind("SetShield", setShield != nullptr, valid);
		CheckBind("WeaponReload", weaponReload != nullptr, valid);
		CheckBind("ContainerBuildResources", containerBuildResources != nullptr, valid);
		CheckBind("EquipmentBuildResources", equipmentBuildResources != nullptr, valid);
		CheckBind("ResolveObject", resolver, valid);

		if (!tradeAccount or !tradeBudgetReturn)
		{
			WriteLog("Trade Account", "signatures missing; override disabled");
		}

		if (!mapInfoBuilder)
		{
			WriteLog("Map Info", "builder signature missing; card-layer control disabled");
		}

		if (!mapInfoEligibility or !mapInfoEligibilityReturn)
		{
			WriteLog("Map Info", "selected gate signatures missing; visibility control disabled");
		}

		if (!mapInfoEligibility or !tradeFilterEligibilityReturn)
		{
			WriteLog("Map Info", "trade gate signatures missing; station-style trade display disabled");
		}

		if (!valid)
		{
			WriteLog("Hook", "native subsystem disabled");
			SetInitFailure("required_binding_missing");

			return false;
		}

		auto status = MH_Initialize();

		if (status != MH_OK)
		{
			const auto detail = std::string(MH_StatusToString(status));
			WriteLog("Hook", "MinHook initialization failed status=" + detail);
			SetInitFailure("minhook_initialization_failed");

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(moveCalc), reinterpret_cast<void*>(MoveCalcHook), reinterpret_cast<void**>(&moveCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "move calc hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(getDefensibleSpeeds), reinterpret_cast<void*>(GetDefensibleSpeedsHook), reinterpret_cast<void**>(&getDefensibleSpeedsCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "defensible speeds hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(theoreticalMove), reinterpret_cast<void*>(TheoreticalMoveHook), reinterpret_cast<void**>(&theoreticalMoveCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "theoretical move hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(addShield), reinterpret_cast<void*>(ShieldHook), reinterpret_cast<void**>(&shieldCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "shield hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(weaponReload), reinterpret_cast<void*>(WeaponReloadHook), reinterpret_cast<void**>(&weaponReloadCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "weapon reload hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(containerBuildResources), reinterpret_cast<void*>(ContainerBuildResourceHook), reinterpret_cast<void**>(&containerBuildResourceCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "container build resource hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		status = MH_CreateHook(reinterpret_cast<void*>(equipmentBuildResources), reinterpret_cast<void*>(EquipmentBuildResourceHook), reinterpret_cast<void**>(&equipmentBuildResourceCall));

		if (status != MH_OK)
		{
			WriteLog("Hook", "equipment build resource hook creation failed status=" + std::string(MH_StatusToString(status)));
			SetInitFailure("hook_creation_failed");
			MH_Uninitialize();

			return false;
		}

		bool tradeHookReady = false;

		if (tradeAccount and tradeBudgetReturn)
		{
			status = MH_CreateHook(reinterpret_cast<void*>(tradeAccount), reinterpret_cast<void*>(TradeAccountHook), reinterpret_cast<void**>(&tradeAccountCall));

			if (status == MH_OK)
			{
				tradeHookReady = true;
			}
			else
			{
				WriteLog("Trade Account", "hook creation failed status=" + std::string(MH_StatusToString(status)));
			}
		}

		bool mapInfoEligibilityHookReady = false;

		if (mapInfoEligibility and (mapInfoEligibilityReturn or tradeFilterEligibilityReturn))
		{
			status = MH_CreateHook(reinterpret_cast<void*>(mapInfoEligibility), reinterpret_cast<void*>(MapInfoEligibilityHook), reinterpret_cast<void**>(&mapInfoEligibilityCall));

			if (status == MH_OK)
			{
				mapInfoEligibilityHookReady = true;
			}
			else
			{
				WriteLog("Map Info", "eligibility hook creation failed status=" + std::string(MH_StatusToString(status)));
			}
		}

		bool mapInfoHookReady = false;

		if (mapInfoBuilder)
		{
			status = MH_CreateHook(reinterpret_cast<void*>(mapInfoBuilder), reinterpret_cast<void*>(MapInfoHook), reinterpret_cast<void**>(&mapInfoCall));

			if (status == MH_OK)
			{
				mapInfoHookReady = true;
			}
			else
			{
				WriteLog("Map Info", "builder hook creation failed status=" + std::string(MH_StatusToString(status)));
			}
		}

		status = MH_EnableHook(MH_ALL_HOOKS);

		if (status != MH_OK)
		{
			const auto detail = std::string(MH_StatusToString(status));
			WriteLog("Hook", "hook activation failed status=" + detail);
			SetInitFailure("hook_activation_failed");
			MH_Uninitialize();

			return false;
		}

		const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		std::ostringstream stream;
		stream << "core ready"
			<< " move_calc=0x" << std::hex << reinterpret_cast<std::uintptr_t>(moveCalc) - image
			<< " theoretical_move=0x" << reinterpret_cast<std::uintptr_t>(theoreticalMove) - image
			<< " defensible_speeds=0x" << reinterpret_cast<std::uintptr_t>(getDefensibleSpeeds) - image
			<< " apply=0x" << reinterpret_cast<std::uintptr_t>(applyMove) - image
			<< " shield_add=0x" << reinterpret_cast<std::uintptr_t>(addShield) - image
			<< " shield_set=0x" << reinterpret_cast<std::uintptr_t>(setShield) - image
			<< " weapon_reload=0x" << reinterpret_cast<std::uintptr_t>(weaponReload) - image
			<< " container_build_resources=0x" << reinterpret_cast<std::uintptr_t>(containerBuildResources) - image
			<< " equipment_build_resources=0x" << reinterpret_cast<std::uintptr_t>(equipmentBuildResources) - image
			<< " resolve=0x" << reinterpret_cast<std::uintptr_t>(resolveObj) - image;
		WriteLog("Hook", stream.str());

		if (tradeHookReady)
		{
			stream.str("");
			stream.clear();
			stream << "hook ready"
				<< " holder=0x" << std::hex << reinterpret_cast<std::uintptr_t>(tradeAccount) - image
				<< " return=0x" << reinterpret_cast<std::uintptr_t>(tradeBudgetReturn) - image;
			WriteLog("Trade Account", stream.str());
		}

		if (mapInfoHookReady or mapInfoEligibilityHookReady)
		{
			stream.str("");
			stream.clear();
			stream << "hooks ready";

			if (mapInfoHookReady)
			{
				stream << " builder=0x" << std::hex << reinterpret_cast<std::uintptr_t>(mapInfoBuilder) - image;
			}

			if (mapInfoEligibilityHookReady)
			{
				stream << " eligibility=0x" << std::hex << reinterpret_cast<std::uintptr_t>(mapInfoEligibility) - image;

				if (mapInfoEligibilityReturn)
				{
					stream << " selected_return=0x" << reinterpret_cast<std::uintptr_t>(mapInfoEligibilityReturn) - image;
				}

				if (tradeFilterEligibilityReturn)
				{
					stream << " trade_return=0x" << reinterpret_cast<std::uintptr_t>(tradeFilterEligibilityReturn) - image;
				}
			}

			WriteLog("Map Info", stream.str());
		}

		return true;
	}

}

extern "C" __declspec(dllexport) int luaopen_enhanced_nomad(void* rawState)
{
	auto* state = static_cast<LuaState*>(rawState);

	if (ready.load(std::memory_order_acquire))
	{
		PushLuaModule(state);

		return 1;
	}

	std::lock_guard guard(initLock);

	if (ready.load(std::memory_order_acquire))
	{
		PushLuaModule(state);

		return 1;
	}

	const auto lua = GetModuleHandleW(L"lua51_64.dll");
	luaText = lua ? reinterpret_cast<LuaText>(GetProcAddress(lua, "lua_tolstring")) : nullptr;
	luaTop = lua ? reinterpret_cast<LuaTop>(GetProcAddress(lua, "lua_gettop")) : nullptr;
	luaNumber = lua ? reinterpret_cast<LuaNumber>(GetProcAddress(lua, "lua_tonumber")) : nullptr;
	luaIsNumber = lua ? reinterpret_cast<LuaIsNumber>(GetProcAddress(lua, "lua_isnumber")) : nullptr;
	luaToBool = lua ? reinterpret_cast<LuaToBool>(GetProcAddress(lua, "lua_toboolean")) : nullptr;
	luaPushBool = lua ? reinterpret_cast<LuaBool>(GetProcAddress(lua, "lua_pushboolean")) : nullptr;
	luaPushCClosure = lua ? reinterpret_cast<LuaPushCClosure>(GetProcAddress(lua, "lua_pushcclosure")) : nullptr;
	luaCreateTable = lua ? reinterpret_cast<LuaCreateTable>(GetProcAddress(lua, "lua_createtable")) : nullptr;
	luaSetField = lua ? reinterpret_cast<LuaSetField>(GetProcAddress(lua, "lua_setfield")) : nullptr;
	const auto pushString = lua ? reinterpret_cast<LuaString>(GetProcAddress(lua, "lua_pushlstring")) : nullptr;

	if (!luaPushBool)
	{
		return 0;
	}

	logPath = GetLogPath();
	StartLog();
	initFailureKind.clear();

	if (!CheckGameCompatibility())
	{
		SetInitFailure("game_mismatch");
		luaPushBool(state, 0);

		if (!pushString)
		{
			return 1;
		}

		pushString(state, initFailureKind.data(), initFailureKind.size());

		return 2;
	}

	HMODULE pinned = nullptr;
	const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
	const auto held = GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&luaopen_enhanced_nomad), &pinned) != FALSE;

	if (!held)
	{
		WriteLog("Init", "module pin failed error=" + std::to_string(GetLastError()));
		SetInitFailure("module_pin_failed");
	}

	const auto loaded = held and Initialize();

	if (loaded)
	{
		PushLuaModule(state);

		return 1;
	}

	luaPushBool(state, 0);

	if (!pushString or initFailureKind.empty())
	{
		return 1;
	}

	pushString(state, initFailureKind.data(), initFailureKind.size());

	return 2;
}

static bool Initialize()
{
	try
	{
		WriteLog("Init", "initialization started");

		if (!InitHooks())
		{
			WriteLog("Init", "initialization failed");

			return false;
		}

		ready.store(true, std::memory_order_release);
		WriteLog("Init", "initialization completed");

		return true;
	}

	catch (const std::exception& error)
	{
		WriteLog("Init", "unexpected failure error=" + std::string(error.what()));
		SetInitFailure("unexpected_initialization_failure");

		return false;
	}

	catch (...)
	{
		WriteLog("Init", "unexpected failure");
		SetInitFailure("unexpected_initialization_failure");

		return false;
	}
}

static bool SetPower(std::uint64_t ship, float weapons, float shields, float engines)
{
	if (!ready.load(std::memory_order_acquire) or ship == 0 or !IsPower(weapons) or !IsPower(shields) or !IsPower(engines))
	{
		return false;
	}

	try
	{
		const auto object = ResolveObject(ship);

		if (!object)
		{
			WriteLog("Power", "rejected ship=" + std::to_string(ship) + " reason=unresolved");

			return false;
		}

		if (!IsNomadShip(object))
		{
			WriteLog("Power", "rejected ship=" + std::to_string(ship) + " reason=non_nomad");

			return false;
		}

		const auto groups = GetShipGroups(object);

		{
			std::unique_lock guard(powerLock);
			enginePower[ship] = engines;
		}

		SetGroupPower(ship, groups, weapons, shields);
		const auto movementApplied = ApplyMovement(object);

		std::ostringstream stream;
		stream << "applied ship=" << ship
			<< " weapons=" << weapons
			<< " shields=" << shields
			<< " engines=" << engines
			<< " groups=" << groups.size()
			<< " engine=" << (movementApplied ? "applied" : "deferred");

		WriteLog("Power", stream.str());

		return true;
	}

	catch (...)
	{
		WriteLog("Power", "apply failed ship=" + std::to_string(ship));

		return false;
	}
}

BOOL APIENTRY DllMain(HMODULE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		module = instance;
		DisableThreadLibraryCalls(instance);
	}

	return TRUE;
}
