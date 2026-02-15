#pragma once

#include <string>
#include <vector>
#include <functional>

class UndoStack {
public:
    void push(const std::string& description,
              std::function<void()> undoFn,
              std::function<void()> redoFn) {
        // Truncate any redo history beyond current position
        if (currentIndex + 1 < (int)stack.size()) {
            stack.resize(currentIndex + 1);
        }
        stack.push_back({description, std::move(undoFn), std::move(redoFn)});
        currentIndex = (int)stack.size() - 1;
    }

    void undo() {
        if (!canUndo()) return;
        stack[currentIndex].undoFn();
        currentIndex--;
    }

    void redo() {
        if (!canRedo()) return;
        currentIndex++;
        stack[currentIndex].redoFn();
    }

    bool canUndo() const { return currentIndex >= 0; }
    bool canRedo() const { return currentIndex + 1 < (int)stack.size(); }

    std::string undoDescription() const {
        if (!canUndo()) return "";
        return stack[currentIndex].desc;
    }

    std::string redoDescription() const {
        if (!canRedo()) return "";
        return stack[currentIndex + 1].desc;
    }

    void clear() {
        stack.clear();
        currentIndex = -1;
    }

private:
    struct Action {
        std::string desc;
        std::function<void()> undoFn;
        std::function<void()> redoFn;
    };
    std::vector<Action> stack;
    int currentIndex = -1;
};
