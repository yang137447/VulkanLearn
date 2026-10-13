#include "editor/command/assetJsonCommandHost.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/command/strictJson.h"

namespace VL::Editor
{
nlohmann::json AssetJsonCommandHost::Process(std::string_view request)
{
    EditorCommandId id = 0;
    try
    {
        if (request.size() > 1024 * 1024) throw std::invalid_argument("Request exceeds 1 MiB");
        const auto wire = ParseStrictJson(request, 1024 * 1024);
        if (wire.is_object() && wire.contains("commandId") && wire.at("commandId").is_number_unsigned())
            id = wire.at("commandId").get<uint64_t>();
        auto command = EditorCommandJsonCodec::DecodeCommand(wire);
        id = command.commandId;
        const auto submitted = bus.Submit(command);
        if (submitted.admission != EditorCommandAdmission::Queued)
            return EditorCommandJsonCodec::EncodeResult(submitted.result);
        for (const auto& queued : bus.Drain())
        {
            auto result = executor.Execute(queued);
            if (const auto* document = std::get_if<EditorAssetDocumentResultPayload>(&result.payload))
                bus.SetDocumentRevision(document->assetPath, document->documentRevision);
            bus.PublishResult(std::move(result));
        }
        return EditorCommandJsonCodec::EncodeResult(*bus.GetResult(id));
    }
    catch (const EditorCommandJsonCodecError& error)
    {
        return {{"protocolVersion", kEditorCommandProtocolVersion}, {"commandId", id},
            {"status", "rejected"}, {"errorCode", GetEditorCommandJsonErrorName(error.Code())},
            {"message", error.what()}, {"payload", nlohmann::json::object()}};
    }
    catch (const std::exception& error)
    {
        return {{"protocolVersion", kEditorCommandProtocolVersion}, {"commandId", id},
            {"status", "rejected"}, {"errorCode", "invalid_payload"},
            {"message", error.what()}, {"payload", nlohmann::json::object()}};
    }
}
} // namespace VL::Editor
