#pragma once

#include "render/editorBridge/materialInstancePreviewTypes.h"

namespace VL::Editor::Preview
{

// P1 forwarder: the preview value protocol is runtime-owned so RenderSystem
// does not have to include an editor implementation header. Remove these
// aliases when the editor sources move in P4.
// The neutral type is named ...DraftPayload to avoid colliding with the
// existing VL::ApplyMaterialInstancePreviewPayload editor-command payload.
using ApplyMaterialInstancePreviewPayload =
    ::VL::ApplyMaterialInstancePreviewDraftPayload;
using ::VL::ApplyMaterialInstancePreviewDraftPayload;
using ::VL::ConnectMaterialInstancePreviewPayload;
using ::VL::DisconnectMaterialInstancePreviewPayload;
using ::VL::MaterialInstancePreviewAdapterCommand;
using ::VL::MaterialInstancePreviewAdapterResult;
using ::VL::MaterialInstancePreviewCommand;
using ::VL::MaterialInstancePreviewCommandPayload;
using ::VL::MaterialInstancePreviewCommandResult;
using ::VL::MaterialInstancePreviewCommandStatus;
using ::VL::MaterialInstancePreviewCommandType;
using ::VL::MaterialInstancePreviewDraft;
using ::VL::MaterialInstancePreviewErrorCode;
using ::VL::MaterialInstancePreviewPathNormalizationResult;
using ::VL::kMaterialInstancePreviewProtocolVersion;
using ::VL::MaterialInstancePreviewResultPayload;
using ::VL::MaterialInstancePreviewState;
using ::VL::MaterialInstancePreviewStatus;
using ::VL::MaterialInstancePreviewWorldIdentity;
using ::VL::NormalizeMaterialInstancePath;
using ::VL::NormalizeWorldIdentity;
using ::VL::NormalizedMaterialInstancePath;
using ::VL::PreviewAdapterFailureStage;
using ::VL::PreviewAdapterOperationStatus;
using ::VL::PreviewCommandId;
using ::VL::PreviewCommandSource;
using ::VL::PreviewOperationId;
using ::VL::RestoreMaterialInstancePreviewBaselinePayload;
using ::VL::ToString;

} // namespace VL::Editor::Preview
