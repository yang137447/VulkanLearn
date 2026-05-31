#pragma once

#include <optional>
#include <string>
#include <utility>

namespace VL
{

// Structured runtime error used by UE-Lite loading and transition code. The
// source fields are optional so call sites can add file/node context when it is
// available without inventing a different error type for each subsystem.
struct RuntimeError
{
    std::string code;
    std::string message;
    std::string sourcePath;
    std::string sourceNode;
};

std::string FormatRuntimeError(const RuntimeError& error);

template <typename T>
class RuntimeResult
{
public:
    static RuntimeResult Success(T value)
    {
        RuntimeResult result;
        result.value = std::move(value);
        return result;
    }

    static RuntimeResult Failure(RuntimeError error)
    {
        RuntimeResult result;
        result.error = std::move(error);
        return result;
    }

    bool IsSuccess() const { return value.has_value(); }
    bool IsFailure() const { return !IsSuccess(); }

    const T& Value() const { return value.value(); }
    T& Value() { return value.value(); }

    const RuntimeError& Error() const { return error.value(); }

private:
    std::optional<T> value;
    std::optional<RuntimeError> error;
};

template <>
class RuntimeResult<void>
{
public:
    static RuntimeResult Success()
    {
        RuntimeResult result;
        result.success = true;
        return result;
    }

    static RuntimeResult Failure(RuntimeError error)
    {
        RuntimeResult result;
        result.error = std::move(error);
        return result;
    }

    bool IsSuccess() const { return success; }
    bool IsFailure() const { return !success; }

    const RuntimeError& Error() const { return error.value(); }

private:
    bool success = false;
    std::optional<RuntimeError> error;
};

inline RuntimeError MakeRuntimeError(
    std::string code,
    std::string message,
    std::string sourcePath = {},
    std::string sourceNode = {})
{
    RuntimeError error;
    error.code = std::move(code);
    error.message = std::move(message);
    error.sourcePath = std::move(sourcePath);
    error.sourceNode = std::move(sourceNode);
    return error;
}

} // namespace VL
