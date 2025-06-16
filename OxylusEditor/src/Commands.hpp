#pragma once

namespace ox {
class Command {
public:
  virtual ~Command() = default;
  virtual void Execute() const = 0;
};

template <typename T>
class UndoCommand : public Command {
public:
  UndoCommand(T oldValue, T* pValue) : m_pValue(pValue), m_OldValue(oldValue) {}

  ~UndoCommand() override = default;

  void Execute() const override { *m_pValue = m_OldValue; }

private:
  T* m_pValue;
  T m_OldValue;
};
} // namespace ox
