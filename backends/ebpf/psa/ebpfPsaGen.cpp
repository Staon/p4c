/*
Copyright 2022-present Orange
Copyright 2022-present Open Networking Foundation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "ebpfPsaGen.h"
#include "ebpfPsaParser.h"
#include "ebpfPsaDeparser.h"
#include "ebpfPsaTable.h"
#include "ebpfPsaControl.h"
#include "xdpHelpProgram.h"

namespace EBPF {

// =====================PSAEbpfGenerator=============================
void PSAEbpfGenerator::emitPSAIncludes(CodeBuilder *builder) const {
    builder->appendLine("#include <stdbool.h>");
    builder->appendLine("#include <linux/if_ether.h>");
    builder->appendLine("#include \"psa.h\"");
}

void PSAEbpfGenerator::emitPreamble(CodeBuilder *builder) const {
    emitCommonPreamble(builder);
    builder->newline();

    // TODO: enable configuring MAX_PORTS/MAX_INSTANCES/MAX_SESSIONS using compiler options.
    builder->appendLine("#define CLONE_MAX_PORTS 64");
    builder->appendLine("#define CLONE_MAX_INSTANCES 1");
    builder->appendLine("#define CLONE_MAX_CLONES (CLONE_MAX_PORTS * CLONE_MAX_INSTANCES)");
    builder->appendLine("#define CLONE_MAX_SESSIONS 1024");
    builder->newline();

    builder->appendLine("#ifndef PSA_PORT_RECIRCULATE\n"
        "#error \"PSA_PORT_RECIRCULATE not specified, "
        "please use -DPSA_PORT_RECIRCULATE=n option to specify index of recirculation "
        "interface (see the result of command 'ip link')\"\n"
        "#endif");
    builder->appendLine("#define P4C_PSA_PORT_RECIRCULATE 0xfffffffa");
    builder->newline();
}

void PSAEbpfGenerator::emitCommonPreamble(CodeBuilder *builder) const {
    builder->newline();
    builder->appendLine("#define EBPF_MASK(t, w) ((((t)(1)) << (w)) - (t)1)");
    builder->appendLine("#define BYTES(w) ((w) / 8)");
    builder->appendLine(
        "#define write_partial(a, w, s, v) do { *((u8*)a) = ((*((u8*)a)) "
        "& ~(EBPF_MASK(u8, w) << s)) | (v << s) ; } while (0)");
    builder->appendLine("#define write_byte(base, offset, v) do { "
                        "*(u8*)((base) + (offset)) = (v); "
                        "} while (0)");
    builder->target->emitPreamble(builder);
}

void PSAEbpfGenerator::emitInternalStructures(CodeBuilder *builder) const {
    builder->appendLine("struct internal_metadata {\n"
                        "    __u16 pkt_ether_type;\n"
                        "} __attribute__((aligned(4)));");
    builder->newline();

    // emit helper struct for clone sessions
    builder->appendLine("struct list_key_t {\n"
                        "    __u32 port;\n"
                        "    __u16 instance;\n"
                        "};\n"
                        "typedef struct list_key_t elem_t;\n"
                        "\n"
                        "struct element {\n"
                        "    struct clone_session_entry entry;\n"
                        "    elem_t next_id;\n"
                        "} __attribute__((aligned(4)));");
    builder->newline();
}

/* Generate headers and structs in p4 prog */
void PSAEbpfGenerator::emitTypes(CodeBuilder *builder) const {
    for (auto type : ebpfTypes) {
        type->emit(builder);
    }

    ingress->parser->emitTypes(builder);
    ingress->control->emitTableTypes(builder);
    egress->parser->emitTypes(builder);
    egress->control->emitTableTypes(builder);
    builder->newline();
}

void PSAEbpfGenerator::emitGlobalHeadersMetadata(CodeBuilder *builder) const {
    builder->append("struct hdr_md ");
    builder->blockStart();
    builder->emitIndent();

    ingress->parser->headerType->declare(builder, "cpumap_hdr", false);
    builder->endOfStatement(true);
    builder->emitIndent();
    auto user_md_type = ingress->typeMap->getType(ingress->control->user_metadata);
    BUG_CHECK(user_md_type != nullptr, "cannot declare user metadata");
    auto userMetadataType = EBPFTypeFactory::instance->create(user_md_type);
    userMetadataType->declare(builder, "cpumap_usermeta", false);
    builder->endOfStatement(true);

    // additional field to avoid compiler errors when both headers and user_metadata are empty.
    builder->emitIndent();
    builder->append("__u8 __hook");
    builder->endOfStatement(true);

    builder->blockEnd(false);
    builder->endOfStatement(true);
    builder->newline();
}

void PSAEbpfGenerator::emitPacketReplicationTables(CodeBuilder *builder) const {
    builder->target->emitMapInMapDecl(builder, "clone_session_tbl_inner",
                                      TableHash, "elem_t",
                                      "struct element", MaxClones, "clone_session_tbl",
                                      TableArray, "__u32", MaxCloneSessions);
    builder->target->emitMapInMapDecl(builder, "multicast_grp_tbl_inner",
                                      TableHash, "elem_t",
                                      "struct element", MaxClones, "multicast_grp_tbl",
                                      TableArray, "__u32", MaxCloneSessions);
}

void PSAEbpfGenerator::emitPipelineInstances(CodeBuilder *builder) const {
    ingress->parser->emitValueSetInstances(builder);
    ingress->control->emitTableInstances(builder);

    egress->parser->emitValueSetInstances(builder);
    egress->control->emitTableInstances(builder);

    builder->target->emitTableDecl(builder, "hdr_md_cpumap",
                                   TablePerCPUArray, "u32",
                                   "struct hdr_md", 2);
}

void PSAEbpfGenerator::emitInitializer(CodeBuilder *builder) const {
    emitInitializerSection(builder);
    builder->appendFormat("int %s()",
                          "map_initializer");
    builder->spc();
    builder->blockStart();
    builder->emitIndent();
    builder->appendFormat("u32 %s = 0;", ingress->zeroKey.c_str());
    builder->newline();
    ingress->control->emitTableInitializers(builder);
    egress->control->emitTableInitializers(builder);
    builder->newline();
    builder->emitIndent();
    builder->appendLine("return 0;");
    builder->blockEnd(true);
}

void PSAEbpfGenerator::emitHelperFunctions(CodeBuilder *builder) const {
    cstring forEachFunc =
            "static __always_inline\n"
            "int do_for_each(SK_BUFF *skb, void *map, "
            "unsigned int max_iter, "
            "void (*a)(SK_BUFF *, void *))\n"
            "{\n"
            "    elem_t head_idx = {0, 0};\n"
            "    struct element *elem = bpf_map_lookup_elem(map, &head_idx);\n"
            "    if (!elem) {\n"
            "        return -1;\n"
            "    }\n"
            "    if (elem->next_id.port == 0 && elem->next_id.instance == 0) {\n"
            "       %trace_msg_no_elements%"
            "        return 0;\n"
            "    }\n"
            "    elem_t next_id = elem->next_id;\n"
            "    for (unsigned int i = 0; i < max_iter; i++) {\n"
            "        struct element *elem = bpf_map_lookup_elem(map, &next_id);\n"
            "        if (!elem) {\n"
            "            break;\n"
            "        }\n"
            "        a(skb, &elem->entry);\n"
            "        if (elem->next_id.port == 0 && elem->next_id.instance == 0) {\n"
            "            break;\n"
            "        }\n"
            "        next_id = elem->next_id;\n"
            "    }\n"
            "    return 0;\n"
            "}";
    if (options.emitTraceMessages) {
        forEachFunc = forEachFunc.replace("%trace_msg_no_elements%",
            "        bpf_trace_message(\"do_for_each: No elements found in list\\n\");\n");
    } else {
        forEachFunc = forEachFunc.replace("%trace_msg_no_elements%", "");
    }
    builder->appendLine(forEachFunc);
    builder->newline();

    // Function to perform cloning, common for ingress and egress
    cstring cloneFunction =
            "static __always_inline\n"
            "void do_clone(SK_BUFF *skb, void *data)\n"
            "{\n"
            "    struct clone_session_entry *entry = (struct clone_session_entry *) data;\n"
                "%trace_msg_redirect%"
            "    bpf_clone_redirect(skb, entry->egress_port, 0);\n"
            "}";
    if (options.emitTraceMessages) {
        cloneFunction = cloneFunction.replace(cstring("%trace_msg_redirect%"),
            "    bpf_trace_message(\"do_clone: cloning pkt, egress_port=%d, cos=%d\\n\", "
            "entry->egress_port, entry->class_of_service);\n");
    } else {
        cloneFunction = cloneFunction.replace(cstring("%trace_msg_redirect%"), "");
    }
    builder->appendLine(cloneFunction);
    builder->newline();

    cstring pktClonesFunc =
            "static __always_inline\n"
            "int do_packet_clones(SK_BUFF * skb, void * map, __u32 session_id, "
                "PSA_PacketPath_t new_pkt_path, __u8 caller_id)\n"
            "{\n"
                "%trace_msg_clone_requested%"
            "    struct psa_global_metadata * meta = (struct psa_global_metadata *) skb->cb;\n"
            "    void * inner_map;\n"
            "    inner_map = bpf_map_lookup_elem(map, &session_id);\n"
            "    if (inner_map != NULL) {\n"
            "        PSA_PacketPath_t original_pkt_path = meta->packet_path;\n"
            "        meta->packet_path = new_pkt_path;\n"
            "        if (do_for_each(skb, inner_map, CLONE_MAX_CLONES, &do_clone) < 0) {\n"
                        "%trace_msg_clone_failed%"
            "            return -1;\n"
            "        }\n"
            "        meta->packet_path = original_pkt_path;\n"
            "    } else {\n"
                    "%trace_msg_no_session%"
            "    }\n"
                "%trace_msg_cloning_done%"
            "    return 0;\n"
            " }";
    if (options.emitTraceMessages) {
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_clone_requested%"),
            "    bpf_trace_message(\"Clone#%d: pkt clone requested, session=%d\\n\", "
            "caller_id, session_id);\n");
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_clone_failed%"),
            "            bpf_trace_message(\"Clone#%d: failed to clone packet\", caller_id);\n");
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_no_session%"),
            "        bpf_trace_message(\"Clone#%d: session_id not found, "
            "no clones created\\n\", caller_id);\n");
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_cloning_done%"),
            "    bpf_trace_message(\"Clone#%d: packet cloning finished\\n\", caller_id);\n");
    } else {
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_clone_requested%"), "");
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_clone_failed%"), "");
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_no_session%"), "");
        pktClonesFunc = pktClonesFunc.replace(cstring("%trace_msg_cloning_done%"), "");
    }

    builder->appendLine(pktClonesFunc);
    builder->newline();
}

// =====================PSAArchTC=============================
void PSAArchTC::emit(CodeBuilder *builder) const {
    /**
     * How the structure of a single C program for PSA should look like?
     * 1. Automatically generated comment
     * 2. Includes
     * 3. Macro definitions (it's called "preamble")
     * 4. Headers, structs, types, PSA-specific data types.
     * 5. BPF map definitions.
     * 6. BPF map initialization
     * 7. XDP helper program.
     * 8. Helper functions
     * 9. TC Ingress program.
     * 10. TC Egress program.
     */

    // 1. Automatically generated comment.
    // Note we use inherited function from EBPFProgram.
    xdp->emitGeneratedComment(builder);

    /*
     * 2. Includes.
     */
    builder->target->emitIncludes(builder);
    emitPSAIncludes(builder);

    /*
     * 3. Macro definitions (it's called "preamble")
     */
    emitPreamble(builder);

    /*
     * 4. Headers, structs, types, PSA-specific data types.
     */
    emitInternalStructures(builder);
    emitTypes(builder);
    emitGlobalHeadersMetadata(builder);

    /*
     * 5. BPF map definitions.
     */
    emitInstances(builder);

    /*
     * 6. BPF map initialization
     */
    emitInitializer(builder);
    builder->newline();

    /*
     * 7. XDP helper program.
     */
    xdp->emit(builder);

    /*
     * 8. Helper functions for ingress and egress program.
     */
    emitHelperFunctions(builder);

    /*
     * 9. TC Ingress program.
     */
    ingress->emit(builder);

    /*
     * 10. TC Egress program.
     */
    egress->emit(builder);

    builder->target->emitLicense(builder, ingress->license);
}

void PSAArchTC::emitInstances(CodeBuilder *builder) const {
    builder->appendLine("REGISTER_START()");

    emitPacketReplicationTables(builder);
    emitPipelineInstances(builder);

    builder->appendLine("REGISTER_END()");
    builder->newline();
}

void PSAArchTC::emitInitializerSection(CodeBuilder *builder) const {
    builder->appendLine("SEC(\"classifier/map-initializer\")");
}

// =====================ConvertToEbpfPSA=============================
const PSAEbpfGenerator * ConvertToEbpfPSA::build(const IR::ToplevelBlock *tlb) {
    /*
     * TYPES
     */
    std::vector<EBPFType*> ebpfTypes;
    for (auto d : tlb->getProgram()->objects) {
        if (d->is<IR::Type>() && !d->is<IR::IContainer>() &&
            !d->is<IR::Type_Extern>() && !d->is<IR::Type_Parser>() &&
            !d->is<IR::Type_Control>() && !d->is<IR::Type_Typedef>() &&
            !d->is<IR::Type_Error>()) {
            if (d->srcInfo.isValid()) {
                auto sourceFile = d->srcInfo.getSourceFile();
                if (sourceFile.endsWith("/psa.p4")) {
                    // do not generate standard PSA types
                    continue;
                }
            }

            auto type = EBPFTypeFactory::instance->create(d->to<IR::Type>());
            if (type == nullptr)
                continue;
            ebpfTypes.push_back(type);
        }
    }

    /*
    * INGRESS
    */
    auto ingress = tlb->getMain()->getParameterValue("ingress")->to<IR::PackageBlock>();
    auto ingressParser = ingress->getParameterValue("ip");
    BUG_CHECK(ingressParser != nullptr, "No ingress parser block found");
    auto ingressControl = ingress->getParameterValue("ig");
    BUG_CHECK(ingressControl != nullptr, "No ingress control block found");
    auto ingressDeparser = ingress->getParameterValue("id");
    BUG_CHECK(ingressDeparser != nullptr, "No ingress deparser block found");

    /*
    * EGRESS
    */
    auto egress = tlb->getMain()->getParameterValue("egress")->to<IR::PackageBlock>();
    auto egressParser = egress->getParameterValue("ep");
    BUG_CHECK(egressParser != nullptr, "No egress parser block found");
    auto egressControl = egress->getParameterValue("eg");
    BUG_CHECK(egressControl != nullptr, "No egress control block found");
    auto egressDeparser = egress->getParameterValue("ed");
    BUG_CHECK(egressDeparser != nullptr, "No egress deparser block found");

    auto xdp = new XDPHelpProgram(options);

    auto ingress_pipeline_converter =
        new ConvertToEbpfPipeline("tc-ingress", TC_INGRESS, options,
            ingressParser->to<IR::ParserBlock>(),
            ingressControl->to<IR::ControlBlock>(),
            ingressDeparser->to<IR::ControlBlock>(),
            refmap, typemap);
    ingress->apply(*ingress_pipeline_converter);
    tlb->getProgram()->apply(*ingress_pipeline_converter);
    auto tcIngress = ingress_pipeline_converter->getEbpfPipeline();

    auto egress_pipeline_converter =
        new ConvertToEbpfPipeline("tc-egress", TC_EGRESS, options,
            egressParser->to<IR::ParserBlock>(),
            egressControl->to<IR::ControlBlock>(),
            egressDeparser->to<IR::ControlBlock>(),
            refmap, typemap);
    egress->apply(*egress_pipeline_converter);
    tlb->getProgram()->apply(*egress_pipeline_converter);
    auto tcEgress = egress_pipeline_converter->getEbpfPipeline();

    return new PSAArchTC(options, ebpfTypes, xdp, tcIngress, tcEgress);
}

const IR::Node *ConvertToEbpfPSA::preorder(IR::ToplevelBlock *tlb) {
    ebpf_psa_arch = build(tlb);
    return tlb;
}

// =====================EbpfPipeline=============================
bool ConvertToEbpfPipeline::preorder(const IR::PackageBlock *block) {
    (void) block;
    if (type == TC_INGRESS) {
        pipeline = new TCIngressPipeline(name, options, refmap, typemap);
    } else if (type == TC_EGRESS) {
        pipeline = new TCEgressPipeline(name, options, refmap, typemap);
    } else {
        ::error(ErrorType::ERR_INVALID, "unknown type of pipeline");
        return false;
    }

    auto parser_converter = new ConvertToEBPFParserPSA(pipeline, refmap, typemap, options, type);
    parserBlock->apply(*parser_converter);
    pipeline->parser = parser_converter->getEBPFParser();
    CHECK_NULL(pipeline->parser);

    auto control_converter = new ConvertToEBPFControlPSA(pipeline,
                                                         pipeline->parser->headers,
                                                         refmap, typemap, options, type);
    controlBlock->apply(*control_converter);
    pipeline->control = control_converter->getEBPFControl();
    CHECK_NULL(pipeline->control);

    auto deparser_converter = new ConvertToEBPFDeparserPSA(
            pipeline,
            pipeline->parser->headers, pipeline->control->outputStandardMetadata,
            refmap, typemap, options, type);
    deparserBlock->apply(*deparser_converter);
    pipeline->deparser = deparser_converter->getEBPFDeparser();
    CHECK_NULL(pipeline->deparser);

    return true;
}

// =====================EBPFParser=============================
bool ConvertToEBPFParserPSA::preorder(const IR::ParserBlock *prsr) {
    auto pl = prsr->container->type->applyParams;

    parser = new EBPFPsaParser(program, prsr, typemap);

    // ingress parser
    unsigned numOfParams = 6;
    if (type == TC_EGRESS) {
        // egress parser
        numOfParams = 7;
    }

    if (pl->size() != numOfParams) {
        ::error(ErrorType::ERR_EXPECTED,
                "Expected parser to have exactly %1% parameters",
                numOfParams);
        return false;
    }

    auto it = pl->parameters.begin();
    parser->packet = *it; ++it;
    parser->headers = *it; ++it;
    parser->user_metadata = *it;
    auto resubmit_meta = *(it + 2);

    for (auto state : prsr->container->states) {
        auto ps = new EBPFParserState(state, parser);
        parser->states.push_back(ps);
    }

    auto ht = typemap->getType(parser->headers);
    if (ht == nullptr)
        return false;
    parser->headerType = EBPFTypeFactory::instance->create(ht);

    parser->visitor->useAsPointerVariable(resubmit_meta->name.name);
    parser->visitor->useAsPointerVariable(parser->user_metadata->name.name);
    parser->visitor->useAsPointerVariable(parser->headers->name.name);

    return true;
}

// =====================EBPFControl=============================
bool ConvertToEBPFControlPSA::preorder(const IR::ControlBlock *ctrl) {
    control = new EBPFControlPSA(program,
                                 ctrl,
                                 parserHeaders);
    program->control = control;
    program->to<EBPFPipeline>()->control = control;
    control->hitVariable = refmap->newName("hit");
    auto pl = ctrl->container->type->applyParams;
    auto it = pl->parameters.begin();
    control->headers = *it; ++it;
    control->user_metadata = *it; ++it;
    control->inputStandardMetadata = *it; ++it;
    control->outputStandardMetadata = *it;

    auto codegen = new ControlBodyTranslator(control);
    codegen->substitute(control->headers, parserHeaders);

    if (type != TC_EGRESS) {
        codegen->useAsPointerVariable(control->outputStandardMetadata->name.name);
    }

    codegen->useAsPointerVariable(control->headers->name.name);
    codegen->useAsPointerVariable(control->user_metadata->name.name);

    control->codeGen = codegen;

    for (auto a : ctrl->constantValue) {
        auto b = a.second;
        if (b->is<IR::Block>()) {
            this->visit(b->to<IR::Block>());
        }
    }
    return true;
}

bool ConvertToEBPFControlPSA::preorder(const IR::TableBlock *tblblk) {
    // use HASH_MAP as default type
    TableKind tableKind = TableHash;

    // If any key field is LPM we will generate an LPM table
    auto keyGenerator = tblblk->container->getKey();
    if (keyGenerator != nullptr) {
        for (auto it : keyGenerator->keyElements) {
            // optimization: check if we should generate timestamp
            if (it->expression->toString().endsWith("timestamp")) {
                control->timestampIsUsed = true;
            }

            auto mtdecl = refmap->getDeclaration(it->matchType->path, true);
            auto matchType = mtdecl->getNode()->to<IR::Declaration_ID>();
            if (matchType->name.name != P4::P4CoreLibrary::instance.exactMatch.name &&
                matchType->name.name != P4::P4CoreLibrary::instance.lpmMatch.name)
                ::error(ErrorType::ERR_UNSUPPORTED,
                        "Match of type %1% not supported", it->matchType);

            if (matchType->name.name == P4::P4CoreLibrary::instance.lpmMatch.name) {
                if (tableKind == TableLPMTrie) {
                    ::error(ErrorType::ERR_UNSUPPORTED,
                            "%1%: only one LPM field allowed", it->matchType);
                    return false;
                }
                tableKind = TableLPMTrie;
            }
        }
    }

    EBPFTablePSA *table = new EBPFTablePSA(program, tblblk, control->codeGen);

    control->tables.emplace(tblblk->container->name, table);
    return true;
}

bool ConvertToEBPFControlPSA::preorder(const IR::Member *m) {
    // the condition covers both ingress and egress timestamp
    if (m->member.name.endsWith("timestamp")) {
        control->timestampIsUsed = true;
    }

    return true;
}

bool ConvertToEBPFControlPSA::preorder(const IR::IfStatement *ifState) {
    if (ifState->condition->is<IR::Equ>()) {
        auto i = ifState->condition->to<IR::Equ>();
        if (i->right->toString().endsWith("timestamp") ||
            i->left->toString().endsWith("timestamp")) {
            control->timestampIsUsed = true;
        }
    }
    return true;
}

bool ConvertToEBPFControlPSA::preorder(const IR::Declaration_Variable* decl) {
    if (type == TC_INGRESS) {
        if (decl->type->is<IR::Type_Name>() &&
            decl->type->to<IR::Type_Name>()->path->name.name == "psa_ingress_output_metadata_t") {
                control->codeGen->useAsPointerVariable(decl->name.name);
        }
    }
    return true;
}

// =====================EBPFDeparser=============================
bool ConvertToEBPFDeparserPSA::preorder(const IR::ControlBlock *ctrl) {
    if (type == TC_INGRESS) {
        deparser = new TCIngressDeparserPSA(program, ctrl, parserHeaders, istd);
    } else if (type == TC_EGRESS) {
        deparser = new TCEgressDeparserPSA(program, ctrl, parserHeaders, istd);
    } else {
        BUG("undefined pipeline type, cannot build deparser");
    }

    if (!deparser->build()) {
        BUG("failed to build deparser");
    }

    deparser->codeGen->substitute(deparser->headers, parserHeaders);
    deparser->codeGen->useAsPointerVariable(deparser->headers->name.name);

    if (type == TC_INGRESS) {
        deparser->codeGen->useAsPointerVariable(deparser->resubmit_meta->name.name);
        deparser->codeGen->useAsPointerVariable(deparser->user_metadata->name.name);
    }

    if (ctrl->container->is<IR::P4Control>()) {
        auto p4Control = ctrl->container->to<IR::P4Control>();
        // TODO: placeholder for handling digests
        this->visit(p4Control->body);
    }

    return false;
}

}  // namespace EBPF
