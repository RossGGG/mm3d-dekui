// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/translate/a32_translate.h>
#include <dynarmic/frontend/A32/translate/translate_callbacks.h>
#include <dynarmic/ir/opt/passes.h>
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "dekui_aot/frontend.h"
#include "dekui_aot/image.h"

namespace DekuiAot {

namespace {

constexpr std::size_t MaxBlockInstructions = 512;

class Callbacks final : public Dynarmic::A32::TranslateCallbacks {
public:
    explicit Callbacks(const Image& image) : image(image) {}

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override {
        return image.ReadCode(vaddr);
    }
    bool PreCodeReadHook(bool, Dynarmic::A32::VAddr, Dynarmic::A32::IREmitter& ir) override {
        // Dynarmic never bounds a block, so data that decodes as straight-line
        // code (zero fill is ANDEQ) would make one block of the whole region,
        // and its condition tracking is quadratic in block length. Splitting a
        // block is invisible to the guest: it continues in the next block.
        if (++instructions > MaxBlockInstructions) {
            ir.SetTerm(Dynarmic::IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        return true;
    }
    void PreCodeTranslationHook(bool, Dynarmic::A32::VAddr, Dynarmic::A32::IREmitter&) override {}
    std::uint64_t GetTicksForCode(bool is_thumb, Dynarmic::A32::VAddr,
                                  std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

private:
    const Image& image;
    std::size_t instructions = 0;
};

} // namespace

std::uint64_t Descriptor(std::uint32_t pc, bool thumb, std::uint32_t fpscr) {
    Dynarmic::A32::PSR cpsr{0};
    cpsr.T(thumb);
    return Dynarmic::A32::LocationDescriptor{pc, cpsr, Dynarmic::A32::FPSCR{fpscr}}.UniqueHash();
}

Dynarmic::IR::Block TranslateBlock(const Image& image, std::uint64_t descriptor) {
    using namespace Dynarmic;
    Callbacks callbacks{image};
    // Citra sets define_unpredictable_behaviour and leaves arch_version (v8) and
    // hook_hint_instructions (false) at their UserConfig defaults.
    const A32::TranslationOptions options{A32::ArchVersion::v8, true, false};
    IR::Block block =
        A32::Translate(A32::LocationDescriptor{IR::LocationDescriptor{descriptor}}, &callbacks, options);

    Optimization::PolyfillPass(block, {});
    Optimization::NamingPass(block);
    Optimization::A32GetSetElimination(block, {.convert_nzc_to_nz = true});
    Optimization::DeadCodeElimination(block);
    Optimization::ConstantPropagation(block);
    Optimization::DeadCodeElimination(block);
    Optimization::IdentityRemovalPass(block);
    Optimization::VerificationPass(block);
    return block;
}

} // namespace DekuiAot
