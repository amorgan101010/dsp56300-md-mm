// Independent architectural expectations: DSP56300FM Rev. 5, table 5-1,
// ABS/NEG/INC/DEC, ASL/ASR, ROL/ROR, long moves and DIV (13-54).
// These oracles use raw DSP bit positions, never host-aligned ALU helpers.
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
using namespace dsp56k;
constexpr uint64_t mask56 = (uint64_t{1} << 56) - 1;
constexpr uint64_t sign56 = uint64_t{1} << 55;
constexpr uint64_t mask48 = (uint64_t{1} << 48) - 1;
constexpr uint64_t guard = 0x11223344556677;

uint64_t raw(const TReg56& r) { return uint64_t(r.var) >> 8; }
void assign(TReg56& r, uint64_t v) { r.var = static_cast<int64_t>(v << 8); }
bool bit(uint64_t v, unsigned n) { return ((v >> n) & 1) != 0; }
int scaling(uint32_t sr) { return (sr & SR_S0) ? 1 : (sr & SR_S1) ? -1 : 0; }

uint32_t arithmeticFlags(uint64_t result, uint32_t before, bool overflow, bool carry)
{
	uint32_t sr = before & ~(CCR_E | CCR_U | CCR_N | CCR_Z | CCR_V | CCR_C);
	const unsigned highFraction = 47 + scaling(before);
	// The integer portion must be entirely zero or entirely one. Express
	// that directly; keep this oracle independent of aligned ALU helpers.
	const uint64_t integer = result >> highFraction;
	const uint64_t allOnes = (uint64_t{1} << (56 - highFraction)) - 1;
	if(integer != 0 && integer != allOnes) sr |= CCR_E;
	if(bit(result, highFraction) == bit(result, highFraction - 1)) sr |= CCR_U;
	if(bit(result, 55)) sr |= CCR_N;
	if(result == 0) sr |= CCR_Z;
	if(overflow) sr |= CCR_V | CCR_L;
	if(carry) sr |= CCR_C;
	return sr;
}

struct State { uint64_t value; uint32_t sr; };
State divide(State d, uint32_t source)
{
	const bool add = bit(d.value, 55) != bit(source, 23);
	const bool overflow = bit(d.value, 55) != bit(d.value, 54);
	const uint64_t divisor = (uint64_t(source) << 24) | (bit(source, 23) ? 0xff000000000000ULL : 0);
	d.value = ((d.value << 1) | ((d.sr & CCR_C) ? 1 : 0)) & mask56;
	d.value = (add ? d.value + divisor : d.value - divisor) & mask56;
	d.sr &= ~(CCR_V | CCR_C);
	if(overflow) d.sr |= CCR_V | CCR_L;
	if(!bit(d.value, 55)) d.sr |= CCR_C;
	return d;
}

struct Fixture
{
	DefaultMemoryValidator validator;
	Memory mem{validator, 0x1000};
	PeripheralsNop px, py;
	DSP dsp{mem, &px, &py};
	Assembler assembler;
	TWord cursor = 0x100;
	TWord firstAdvancePC = 0;

	Fixture(bool optimize, unsigned blockSize)
	{
		auto config = dsp.getJit().getConfig();
		config.maxInstructionsPerBlock = blockSize;
		config.linkJitBlocks = true;
		config.enableOptimizer = optimize;
		dsp.getJit().setConfig(config);
		assign(dsp.regs().a, guard);
		assign(dsp.regs().b, guard);
		dsp.regs().x.var = 0x654321123456;
		dsp.regs().y.var = 0x123456abcdef;
		dsp.regs().r[0].var = 0x10;
		mem.set(MemArea_X, 0x10, 0xabcdef);
		mem.set(MemArea_Y, 0x10, 0x123456);
	}

	void emit(const std::string& text)
	{
		const auto op = assembler.assemble(text.c_str());
		if(!op.success()) throw std::string("Manual test assembly failed: ") + text;
		for(unsigned i = 0; i < op.wordCount; ++i) mem.set(MemArea_P, cursor++, op.word[i]);
	}

	void run(bool jit)
	{
		emit("jmp $300");
		cursor = 0x300;
		emit("jmp $300");
		dsp.setPC(0x100);
		dsp.getJit().checkModeChange();
		// No register/CCR reads while the program executes. The PC read neither
		// materializes CCR nor introduces a generated block boundary.
		for(unsigned dispatch = 0; dispatch < 512 && dsp.getPC() != 0x300; ++dispatch)
			{
			if(jit) dsp.execJit(); else dsp.execInterpreter();
			if(!firstAdvancePC && dsp.getPC() != 0x100) firstAdvancePC = dsp.getPC().var;
		}
		if(dsp.getPC() != 0x300) throw std::string("Manual test did not reach exit");
	}
};

unsigned total = 0, failures = 0, displayed = 0;
void check(bool ok, const std::string& label, bool jit, bool optimize, uint64_t input,
	uint32_t initial, uint64_t actual, uint32_t sr, State expected)
{
	++total;
	if(ok) return;
	++failures;
	if(++displayed <= 12)
		std::cerr << "Manual failure " << label << " jit=" << jit << " optimizer=" << optimize
			<< std::hex << " input=" << input << '/' << initial << " actual=" << actual << '/' << sr
			<< " expected=" << expected.value << '/' << expected.sr << std::dec << '\n';
}

void divideContexts(bool interpreterOnly)
{
	// Valid fractional operands, including the actual MD mixer input. Overflow
	// probes execute one iteration and make no claim about a valid quotient.
	struct Input { uint64_t value; uint32_t divisor; unsigned count; };
	const Input inputs[] = {{0x0821d0000000, 0x40d249, 24}, {0x100000000000, 0xc00000, 24},
		{1, 0x800000, 24}, {0x40000000000000, 0x400000, 1}, {0x80000000000000, 0x400000, 1}};
	struct Branch { const char* name; uint32_t flag; };
	const Branch branches[] = {{"jcs", CCR_C}, {"jset #1,sr,", CCR_V}, {"jls", CCR_L}, {"jmi", CCR_N}};
	for(const auto& input : inputs) for(unsigned flags = 0; flags < 8; ++flags)
		for(bool prefix : {false, true}) for(bool useB : {false, true})
		for(unsigned layout = 0; layout < 3; ++layout) for(bool optimize : {false, true})
		for(bool jit : {false, true}) for(const auto& branch : branches)
	{
		if(interpreterOnly && (jit || optimize || layout == 0)) continue;
		const uint32_t initial = ((flags & 1) ? CCR_C : 0) | ((flags & 2) ? CCR_V : 0)
			| ((flags & 4) ? CCR_L : 0) | ((flags & 1) ? CCR_E | CCR_U | CCR_N | CCR_Z | CCR_S : 0);
		auto f = std::make_unique<Fixture>(optimize, layout == 0 ? 1 : 64);
		auto& r = f->dsp.regs();
		assign(useB ? r.b : r.a, input.value);
		// The other accumulator supplies pending E/U/N/Z in the prefix variant.
		const uint64_t other = (flags & 1) ? 0 : sign56;
		assign(useB ? r.a : r.b, other);
		r.x.var = input.divisor;
		r.sr.var = initial;
		State expected{input.value, initial};
		if(prefix)
		{
			f->emit(useB ? "tst a" : "tst b");
			expected.sr = arithmeticFlags(other, initial, false, (initial & CCR_C) != 0);
		}
		if(layout == 2) f->emit("rep #$" + std::string(input.count == 24 ? "18" : "1"));
		for(unsigned i = 0; i < (layout == 2 ? 1 : input.count); ++i) f->emit(useB ? "div x0,b" : "div x0,a");
		for(unsigned i = 0; i < input.count; ++i) expected = divide(expected, input.divisor);
		const TWord afterDiv = f->cursor;
		f->emit(std::string(branch.name) + (branch.flag == CCR_V ? "$200" : " $200"));
		f->emit("move #>1,x1");
		f->emit("jmp $300");
		f->cursor = 0x200;
		f->emit("move #>2,x1");
		f->run(jit);
		const auto value = raw(useB ? r.b : r.a);
		const auto sr = f->dsp.getSR().var;
		const auto marker = (expected.sr & branch.flag) ? 2u : 1u;
		const bool ok = (!jit || layout != 1 || f->firstAdvancePC >= afterDiv)
			&& value == expected.value && sr == expected.sr
			&& raw(useB ? r.a : r.b) == other && r.x.var == (uint64_t(marker) << 24 | input.divisor)
			&& r.y.var == 0x123456abcdef && f->mem.get(MemArea_X, 0x10) == 0xabcdef
			&& f->mem.get(MemArea_Y, 0x10) == 0x123456;
		check(ok, "DIV layout=" + std::to_string(layout) + " prefix=" + std::to_string(prefix)
			+ " " + branch.name + (useB ? " B" : " A"), jit, optimize, input.value, initial, value, sr, expected);
	}
}

// Reduced regression: DIV is unnecessary to lose pending flags on the untaken
// register-bit branch. Check both branch outcomes and pending/explicit bits.
void statusBranches(bool interpreterOnly)
{
	for(const auto* branch : {"jset", "jclr"}) for(unsigned bitIndex : {1u, 3u})
		for(uint64_t input : {uint64_t{0}, sign56}) for(bool useB : {false, true})
		for(bool optimize : {false, true}) for(bool jit : {false, true})
	{
		if(interpreterOnly && (jit || optimize)) continue;
		auto f = std::make_unique<Fixture>(optimize, 64);
		auto& r = f->dsp.regs();
		assign(useB ? r.b : r.a, input);
		r.sr.var = CCR_C | CCR_V;
		const State expected{input, arithmeticFlags(input, CCR_C | CCR_V, false, true)};
		f->emit(useB ? "tst b" : "tst a");
		f->emit(std::string(branch) + " #" + std::to_string(bitIndex) + ",sr,$200");
		f->emit("move #>1,x1"); f->emit("jmp $300");
		f->cursor = 0x200; f->emit("move #>2,x1"); f->run(jit);
		const bool set = (expected.sr & (1u << bitIndex)) != 0;
		const uint64_t marker = (set == (std::string(branch) == "jset")) ? 2 : 1;
		const auto value = raw(useB ? r.b : r.a); const auto sr = f->dsp.getSR().var;
		check(value == expected.value && sr == expected.sr && r.x.var == ((marker << 24) | 0x123456)
			&& raw(useB ? r.a : r.b) == guard, std::string("TST/") + branch + " bit=" + std::to_string(bitIndex),
			jit, optimize, input, CCR_C | CCR_V, value, sr, expected);
	}
}

State arithmetic(const std::string& op, uint64_t input, uint32_t sr, unsigned count)
{
	uint64_t result = input;
	bool carry = (sr & CCR_C) != 0, overflow = false;
	if(op == "abs" || op == "neg")
	{
		if(op == "neg" || bit(input, 55)) result = (uint64_t{0} - input) & mask56;
		overflow = input == sign56;
	}
	else if(op == "inc") { result = (input + 1) & mask56; carry = input == mask56; overflow = input == sign56 - 1; }
	else if(op == "dec") { result = (input - 1) & mask56; carry = input == 0; overflow = input == sign56; }
	else
	{
		carry = false;
		// Repeated one-bit steps make intermediate ASL overflow explicit and
		// avoid reproducing either backend's host-width shift algorithm.
		for(unsigned i = 0; i < count; ++i)
			if(op == "asl")
			{
				carry = bit(result, 55);
				result = (result << 1) & mask56;
				overflow |= carry != bit(result, 55);
			}
			else { carry = bit(result, 0); result = (result >> 1) | (result & sign56); }
	}
	return {result, arithmeticFlags(result, sr, overflow, carry)};
}

void boundaries(bool interpreterOnly)
{
	const uint64_t values[] = {0, 1, mask56, sign56 - 1, sign56, sign56 + 1,
		0x40000000000000, 0xc0000000000000, 0x800000000000, 0x7fffffffffff, 0xff800000000000};
	struct Operation { const char* name; unsigned count; bool variable; };
	const Operation operations[] = {{"abs", 0, false}, {"neg", 0, false}, {"inc", 0, false}, {"dec", 0, false},
		{"asl", 0, false}, {"asr", 0, false}, {"asl", 2, false}, {"asr", 1, false},
		{"asl", 55, false}, {"asr", 55, false}, {"asl", 2, true}, {"asr", 1, true}, {"asl", 0, true}, {"asr", 0, true}, {"asl", 55, true}, {"asr", 55, true}};
	for(const auto& op : operations) for(auto input : values) for(auto mode : {0u, uint32_t(SR_S0), uint32_t(SR_S1)})
		for(auto flags : {0u, 0xffu}) for(bool useB : {false, true}) for(bool jit : {false, true})
	{
		if(interpreterOnly && jit) continue;
		auto f = std::make_unique<Fixture>(true, 64);
		auto& r = f->dsp.regs();
		assign(useB ? r.b : r.a, input);
		r.sr.var = mode | flags;
		const std::string dest = useB ? "b" : "a";
		std::string instruction = std::string(op.name) + " " + dest;
		if(std::string(op.name) == "asl" || std::string(op.name) == "asr")
		{
			// Upper 18 source bits are ignored; low six contain the shift count.
			r.x.var = 0x654321ffffc0ULL | op.count;
			instruction = std::string(op.name) + " " + (op.variable ? "x0" : "#" + std::to_string(op.count)) + "," + dest + "," + dest;
		}
		const uint64_t beforeX = r.x.var;
		const auto expected = arithmetic(op.name, input, mode | flags, op.count);
		f->emit(instruction);
		f->run(jit);
		const auto value = raw(useB ? r.b : r.a);
		const auto sr = f->dsp.getSR().var;
		check(value == expected.value && sr == expected.sr && raw(useB ? r.a : r.b) == guard
			&& r.x.var == beforeX && r.y.var == 0x123456abcdef && r.r[0].var == 0x10
			&& f->mem.get(MemArea_X, 0x10) == 0xabcdef && f->mem.get(MemArea_Y, 0x10) == 0x123456,
			instruction, jit, true, input, mode | flags, value, sr, expected);
	}
	for(const auto* op : {"rol", "ror"}) for(uint64_t field : {0ULL, 1ULL, 0x400000ULL, 0x800000ULL, 0xffffffULL})
		for(auto flags : {0u, uint32_t(CCR_C), 0xfeu, 0xffu}) for(bool useB : {false, true}) for(bool jit : {false, true})
	{
		if(interpreterOnly && jit) continue;
		auto f = std::make_unique<Fixture>(true, 64);
		auto& r = f->dsp.regs();
		const uint64_t input = 0x55000000abcdef | (field << 24);
		assign(useB ? r.b : r.a, input);
		r.sr.var = flags;
		const bool left = std::string(op) == "rol";
		const uint64_t resultField = left ? ((field << 1) | ((flags & CCR_C) ? 1 : 0)) & 0xffffff
			: (field >> 1) | ((flags & CCR_C) ? 0x800000 : 0);
		State expected{(input & ~uint64_t(0xffffff000000)) | (resultField << 24), flags & ~(CCR_N | CCR_Z | CCR_V | CCR_C)};
		if(bit(resultField, 23)) expected.sr |= CCR_N;
		if(!resultField) expected.sr |= CCR_Z;
		if(bit(field, left ? 23 : 0)) expected.sr |= CCR_C;
		const auto instruction = std::string(op) + (useB ? " b" : " a");
		f->emit(instruction); f->run(jit);
		const auto value = raw(useB ? r.b : r.a); const auto sr = f->dsp.getSR().var;
		check(value == expected.value && sr == expected.sr && raw(useB ? r.a : r.b) == guard
			&& r.x.var == 0x654321123456 && r.y.var == 0x123456abcdef,
			instruction, jit, true, input, flags, value, sr, expected);
	}
}

void transfers(bool interpreterOnly)
{
	for(auto mode : {0u, uint32_t(SR_S0), uint32_t(SR_S1)})
	{
		const int offset = scaling(mode);
		const int64_t threshold = int64_t{1} << (47 + offset);
		std::vector<int64_t> inputs = {0, 1, -1, int64_t(sign56 - 1), -int64_t(sign56)};
		for(int delta : {-2, -1, 0, 1, 2}) { inputs.push_back(threshold + delta); inputs.push_back(-threshold + delta); }
		// Also exercise the S-bit growth detector, distinct from overflow/L.
		inputs.push_back(int64_t{1} << (46 - offset));
		for(auto signedInput : inputs) for(bool otherGrowth : {false, true}) for(auto flags : {0u, 0xffu}) for(bool useB : {false, true})
			for(const auto* address : {"l:<$10", "l:(r0)"}) for(bool jit : {false, true})
		{
			if(interpreterOnly && jit) continue;
			auto f = std::make_unique<Fixture>(true, 64);
			auto& r = f->dsp.regs();
			const auto input = uint64_t(signedInput) & mask56;
			assign(useB ? r.b : r.a, input);
			const uint64_t other = otherGrowth ? uint64_t{1} << (46 - offset) : 0;
			assign(useB ? r.a : r.b, other);
			r.sr.var = mode | flags;
			int64_t scaled = signedInput;
			if(offset < 0) scaled *= 2;
			if(offset > 0) scaled = signedInput >= 0 ? signedInput / 2 : -((-signedInput + 1) / 2);
			uint32_t expectedSR = mode | flags;
			if(otherGrowth || bit(input, 46 - offset) != bit(input, 45 - offset)) expectedSR |= CCR_S;
			if(scaled < -int64_t(uint64_t{1} << 47)) { scaled = -int64_t(uint64_t{1} << 47); expectedSR |= CCR_L; }
			if(scaled > int64_t((uint64_t{1} << 47) - 1)) { scaled = int64_t((uint64_t{1} << 47) - 1); expectedSR |= CCR_L; }
			const uint64_t stored = uint64_t(scaled) & mask48;
			const std::string instruction = std::string("move ") + (useB ? "b," : "a,") + address;
			f->emit(instruction); f->run(jit);
			const auto value = raw(useB ? r.b : r.a); const auto sr = f->dsp.getSR().var;
			check(value == input && sr == expectedSR && raw(useB ? r.a : r.b) == other
				&& r.x.var == 0x654321123456 && r.y.var == 0x123456abcdef && r.r[0].var == 0x10
				&& f->mem.get(MemArea_X, 0x10) == stored >> 24 && f->mem.get(MemArea_Y, 0x10) == (stored & 0xffffff),
				instruction, jit, true, input, mode | flags, value, sr, {input, expectedSR});
		}
	}
}
}

int runManualAccumulatorTests(bool interpreterOnly)
{
	// Literal sanity checks also protect the oracle itself across toolchains.
	// The older AppleClang CI build produced incorrect E expectations with
	// the previous per-bit boolean reduction, despite correct emulator output.
	struct OracleCase { uint64_t result; uint32_t mode; uint32_t flags; };
	const OracleCase oracleCases[] = {{0, 0, 0x14}, {mask56, 0, 0x18},
		{0xc0000000000000, 0, 0x38}, {0xff800000000000, SR_S1, 0x838},
		{0xff800000000000, 0, 0x08}, {0x00800000000000, 0, 0x20}};
	for(const auto& test : oracleCases)
		if(arithmeticFlags(test.result, test.mode, false, false) != test.flags)
			throw std::string("Manual flag oracle failed its literal sanity check");

	total = failures = displayed = 0;
	divideContexts(interpreterOnly);
	std::cerr << "Manual DIV contexts: " << total << " cases, " << failures << " failures\n";
	displayed = 0;
	statusBranches(interpreterOnly);
	boundaries(interpreterOnly);
	displayed = 0;
	transfers(interpreterOnly);
	std::cerr << "Manual total: " << total << " cases, " << failures << " failures\n";
	return failures ? 1 : 0;
}
