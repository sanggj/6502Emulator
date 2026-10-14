#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct AssemblerError {
    int line = 0;
    std::string message;
    int column = 0;
    std::string fileName;
};

// Warning categories (bitmask values).
constexpr int WARN_GENERIC = 1;
constexpr int WARN_PAGE_CROSS = 2;
constexpr int WARN_UNOFFICIAL = 4;

struct AssemblerWarning {
    int line = 0;
    int column = 0;
    std::string message;
    int category = 0;
};

struct AssemblerListingLine {
    uint16_t address = 0;
    std::vector<uint8_t> bytes;
    int sourceLine = 0;
    std::string source;
};

struct AssembledProgram {
    std::array<uint8_t, 0x10000> memory{};
    std::vector<bool> written = std::vector<bool>(0x10000, false);
    uint16_t entryPoint = 0x8000;
    uint16_t lowestAddress = 0xFFFF;
    uint16_t highestAddress = 0x0000;
    std::size_t byteCount = 0;
    std::map<std::string, uint16_t> symbols;
    std::map<int, uint16_t> lineAddresses;
    std::vector<AssemblerListingLine> listing;

    struct Segment {
        uint16_t start = 0;
        uint16_t end = 0;
    };
    // One entry per contiguous emitted region (adjacent regions merged), sorted by address.
    std::vector<Segment> segments;
    // Normalized symbol name -> sorted 1-based source lines that reference it in operands/expressions.
    std::map<std::string, std::vector<int>> symbolReferences;
    // Normalized symbol name -> 1-based source line of its definition.
    std::map<std::string, int> symbolDefinitionLines;
    std::vector<AssemblerWarning> warnings;

    std::vector<uint8_t> flatImage() const;
};

struct AssembleOptions {
    std::string basePath;
    std::size_t maxIncludeDepth = 8;
    std::size_t maxExpansionLines = 200000;
    // Directories searched for .include files after the including file's directory.
    std::vector<std::string> includePaths;
    // Bitmask of WARN_* categories to emit; warnings never fail assembly.
    int warnLevel = 7;
    // Symbols defined before pass 1, exactly like ".define NAME value".
    std::map<std::string, int> predefinedSymbols;
};

class Assembler {
  public:
    bool assemble(const std::string &source, AssembledProgram &program, std::vector<AssemblerError> &errors,
                  const AssembleOptions &options = {}) const;

    // Multi-file assembly: all files share one global symbol namespace ("linking" by
    // concatenation). pair = (fileName, sourceText). Errors carry fileName plus the
    // line number within that file when it can be determined.
    bool assembleProject(const std::vector<std::pair<std::string, std::string>> &files,
                         AssembledProgram &program, std::vector<AssemblerError> &errors,
                         const AssembleOptions &options = {}) const;

    static bool parseValue(const std::string &text, uint16_t &value,
                           const std::map<std::string, uint16_t> &symbols = {});
    static std::string normalizeSymbol(const std::string &name);
    static bool exportSymbolsJson(const AssembledProgram &program, const std::string &path,
                                  std::string &error);
    static bool writeListingFile(const AssembledProgram &program, const std::string &path,
                                 std::string &error);
    static bool writeMapFile(const AssembledProgram &program, const std::string &path, std::string &error);
    static bool writeXrefFile(const AssembledProgram &program, const std::string &path, std::string &error);
    static bool exportPrg(const AssembledProgram &program, const std::string &path, std::string &error);
    static bool exportNes(const AssembledProgram &program, const std::string &path, std::string &error,
                          int prgBanks16 = 2);
    static std::map<std::string, int> splitDefines(const std::string &csv);

  public:
    struct ParsedLine {
        std::string label;
        std::string mnemonic;
        std::string operands;
        bool hasLabel = false;
        int number = 0;
    };

    enum class AddrMode {
        Implied,
        Accumulator,
        Immediate,
        ZeroPage,
        ZeroPageX,
        ZeroPageY,
        Absolute,
        AbsoluteX,
        AbsoluteY,
        Indirect,
        IndexedIndirect,
        IndirectIndexed,
        Relative,
        IndirectAbsoluteX,
        Invalid
    };

    struct OpcodeInfo {
        const char *name;
        AddrMode mode;
        uint8_t opcode;
        uint8_t bytes;
        uint8_t cycles;
        bool pagePenalty;
    };

    static const OpcodeInfo *opcodeTable();
    static std::size_t opcodeTableSize();
    static const OpcodeInfo *findOpcode(const std::string &mnemonic, AddrMode mode);
    static const OpcodeInfo *findOpcodeByByte(uint8_t opcode);
    static bool mnemonicExists(const std::string &mnemonic);
    static ParsedLine parseLine(const std::string &line);
    static std::vector<std::string> splitOperands(const std::string &text);
    static bool evalExpression(const std::string &text, uint16_t &value,
                               const std::map<std::string, uint16_t> &symbols, std::string &error);
    static bool evalAtom(const std::string &text, uint16_t &value,
                         const std::map<std::string, uint16_t> &symbols, std::string &error);
    static AddrMode detectMode(const ParsedLine &line, const std::map<std::string, uint16_t> &symbols,
                               std::string &error);
    static int modeSize(AddrMode mode);
    static bool encodeInstruction(const ParsedLine &line, AddrMode mode, uint16_t address,
                                  const std::map<std::string, uint16_t> &symbols, std::vector<uint8_t> &bytes,
                                  std::string &error);
    static bool parseStringLiteral(const std::string &text, std::vector<uint8_t> &out, std::string &error);
};
