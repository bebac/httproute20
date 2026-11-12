#ifndef HTTPPROUTE20_CONTEXT_H
#define HTTPPROUTE20_CONTEXT_H

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace http
{
  // A type-erased scope of named services — a per-request key/value
  // channel with the type as the key. Entries are stored by type:
  // set<T>(shared_ptr<T>) registers a service under T, get<T>() looks
  // it up. A context can have a parent and get walks child -> parent,
  // so a per-request child scope can shadow app-level entries.
  //
  // This is a dependency-injection scope, not a protocol-level
  // connection context.
  //
  // A context is a handle to its scope, like shared_ptr: constness of
  // the context object does not propagate to the entries. get<T>()
  // returns a mutable T* from a const context, and set is const for the
  // same reason, so the const context* that request_like guarantees is
  // enough for middleware to register per-request entries through
  // req.context()->set<T>(...).
  //
  // The application owns the root context (router::set_context); the
  // server attaches a fresh child to every request. The child's
  // lifetime is the request's and it is owned by a single task, so no
  // locking is needed there; app-level entries are shared across
  // requests and must be immutable or externally synchronized.
  class context
  {
  public:
    explicit context(std::shared_ptr<context> parent = nullptr) : parent_(std::move(parent))
    {
    }

    // Registers a service under its (interface) type T in this scope only
    // — parents are not affected. Const like get: a context is a handle
    // to its scope, so a const context* can still register entries.
    template<typename T>
    void set(std::shared_ptr<T> value) const
    {
      entries_[std::type_index(typeid(T))] = std::move(value);
    }

    // Looks T up in this scope, then walks the parent chain. Returns
    // nullptr when no scope in the chain has an entry for T.
    template<typename T>
    T* get() const
    {
      if (const auto it = entries_.find(std::type_index(typeid(T))); it != entries_.end())
        return static_cast<T*>(it->second.get());
      return parent_ ? parent_->get<T>() : nullptr;
    }

    const std::shared_ptr<context>& parent() const
    {
      return parent_;
    }

  private:
    mutable std::unordered_map<std::type_index, std::shared_ptr<void>> entries_;
    std::shared_ptr<context> parent_;
  };
} // namespace http

#endif // HTTPPROUTE20_CONTEXT_H
