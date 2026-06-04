# C++ & Qt — AI Agent Failure Modes

A catalogue of mistakes AI coding assistants make repeatedly on this codebase. Skim this before asking an agent to write engine or desktop code; reread it after a confusing model error.

> The point is not that these tools are bad. The point is that C++ and Qt have invisible failure modes (UB, MOC, lifetime, linker), and reviewing for *these specific patterns* costs nothing once you know them.

---

## Why C++ Is Especially Hard for LLMs

| What | Why it bites the model |
|---|---|
| **Undefined behavior** | Compiles cleanly, runs "fine," corrupts memory hours later. The model can't see UB the way it can see a Python `TypeError`. |
| **Ownership in the type signature** | `T*` could be owned, borrowed, or non-owning. Convention, not type. The model guesses. |
| **Header / source split** | A `.cpp` works, the `.h` is wrong. Two places to make mistakes; many models update only one. |
| **Linker errors** | "undefined reference to vtable for X" surfaces in CI, not at edit time. The model gets no signal. |
| **Template error walls** | A 500-line "no matching function" from one missing `const`. Models patch the symptom, not the root. |
| **Preprocessor + macros** | `Q_OBJECT` invents code; `assert` vanishes in release; `#ifdef` branches the model can't see at once. |
| **Dialect drift** | C++17 / 20 / 23 / 26 idioms differ enough that a "modern C++" answer is often wrong for *your* compiler. |

Qt adds its own layer: MOC code generation, parent-ownership rules, signal/slot semantics, QML bindings, the Qt 5 → 6 transition that pollutes training data.

---

## Top Failure Modes — Engine / CLI (C++23)

### 1. `Q_OBJECT` omitted

The class compiles. Signals never fire. Slots are unreachable. MOC silently skips the class.

```cpp
// Wrong
class Worker : public QObject {
    // <-- missing Q_OBJECT
public:
signals:
    void finished();
};

// Right
class Worker : public QObject {
    Q_OBJECT
public:
signals:
    void finished();
};
```

**Catch:** `qmllint` and the post-edit grep hook. CI also catches it via link error if signals are connected.

### 2. Parented `QObject` placed in `unique_ptr`

Double-free. Crashes on shutdown, sometimes on the parent's destruction.

```cpp
// Wrong
auto button = std::make_unique<QPushButton>(parent);   // both will delete

// Right
auto* button = new QPushButton(parent);                 // parent owns
// or
auto button = std::make_unique<QPushButton>();         // no parent
```

### 3. Lambda captured `this` without a receiver

When `this` is destroyed but the signal source is not, the lambda fires and crashes.

```cpp
// Wrong
connect(timer, &QTimer::timeout, [this]{ tick(); });

// Right
connect(timer, &QTimer::timeout, this, [this]{ tick(); });
```

### 4. `std::expected` "and_then" / "or_else" fabrications

Models invent overloads that don't exist. Common: passing a non-callable, or a callable returning the wrong type, to `.and_then` / `.or_else`.

```cpp
// Wrong: .and_then expects a callable returning std::expected<U, E>
return parse(in).and_then(extract_id);          // if extract_id returns int, this fails to compile

// Right
return parse(in).and_then([](User u) -> std::expected<int, ErrorCode> {
    return u.id;
});
```

### 5. Header-only function definition without `inline`

Multiple-definition linker error when the header is included from two `.cpp`s.

```cpp
// Foo.h — Wrong
int square(int n) { return n * n; }   // ODR violation

// Right
inline int square(int n) { return n * n; }
// or
constexpr int square(int n) { return n * n; }
// or move definition to Foo.cpp
```

### 6. Returning a reference to a temporary

Compiles, runs, dangles.

```cpp
// Wrong
const std::string& name() const {
    return std::format("{} {}", first_, last_);   // temporary
}

// Right
std::string name() const {
    return std::format("{} {}", first_, last_);
}
```

### 7. Iterator invalidation after `push_back`

```cpp
// Wrong
auto it = vec.begin();
vec.push_back(x);              // may reallocate; it is now dangling
*it = y;                       // UB

// Right — reserve before, or re-acquire after
vec.reserve(expected_size);
// ...
vec.push_back(x);
auto it = vec.end() - 1;
```

### 8. `[[assume(x)]]` used as `assert`

`[[assume]]` is a hint to the optimiser. If the assumption is false, you get UB, not a failed test.

```cpp
// Wrong — silent UB if x < 0
[[assume(x >= 0)]];
return std::sqrt(x);

// Right — actual check
if (x < 0) return std::unexpected(ErrorCode::SchemaInvalid);
return std::sqrt(x);
```

### 9. C-style cast hides errors

```cpp
// Wrong
auto* derived = (Derived*)base;     // silently wrong if base isn't Derived

// Right
auto* derived = dynamic_cast<Derived*>(base);
if (!derived) return std::unexpected(ErrorCode::SchemaInvalid);
```

### 10. Using `printf` or `std::cout`

Project convention is `std::print` / `std::println` (C++23). `printf` is a CVE class; `cout` is slow and noisy.

```cpp
// Wrong
std::cout << "user=" << name << " score=" << score << std::endl;

// Right
std::println("user={} score={}", name, score);
```

### 11. Data race on a `RunContext` accessed from two threads

`RunContext` is move-only and lives one run. Models sometimes hand a pointer to a worker thread.

```cpp
// Wrong
auto* ctx_ptr = &ctx;
QtConcurrent::run([ctx_ptr] { ctx_ptr->record(...); });   // race

// Right — confine RunContext to one thread; signal results back
QtConcurrent::run([copy = std::move(local_state)] {
    return process(copy);
});
```

### 12. Forgetting `noexcept` on move ops

`std::vector<T>` falls back to copy on growth if `T`'s move is not `noexcept`. Silent perf regression.

```cpp
// Wrong
class Foo {
    Foo(Foo&&);
    Foo& operator=(Foo&&);
};

// Right
class Foo {
    Foo(Foo&&) noexcept;
    Foo& operator=(Foo&&) noexcept;
};
```

### 13. C-string concatenation into shell or exec

```cpp
// Wrong — command injection if `url` is user-supplied
QProcess::execute(QString("curl ") + url);

// Right — argument list, no shell
QProcess process;
process.start("curl", QStringList{url});
process.waitForFinished();
```

### 14. Manual `delete` on `nlohmann::json::parse` result

`nlohmann::json` is a value type. Writing `new`/`delete` around it is always wrong.

```cpp
// Wrong
auto* j = new nlohmann::json(nlohmann::json::parse(text));
// ...
delete j;

// Right
auto j = nlohmann::json::parse(text);
```

### 15. `find_package(Qt6 ...)` for UI components in engine CMake

Boundary violation. CI's `reqloom_forbid_dependencies` catches it, but only after the configure step.

```cmake
# Wrong — engine/CMakeLists.txt
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Right — engine has Core only; Widgets goes in desktop/CMakeLists.txt
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
```

---

## Top Failure Modes — Desktop / Qt 6.8

### 16. String-form `connect(SIGNAL(...), SLOT(...))`

Not compile-checked. A typo silently fails at runtime.

```cpp
// Wrong
connect(button, SIGNAL(clicked()), this, SLOT(onClicked()));

// Right
connect(button, &QPushButton::clicked, this, &MainWindow::onClicked);
```

### 17. Modifying a model without `beginInsertRows` / `endInsertRows`

Silently corrupts QML views. No error, no warning. The view shows stale data.

```cpp
// Wrong
void TaskModel::add(Task t) {
    tasks_.push_back(t);                    // view doesn't know
}

// Right
void TaskModel::add(Task t) {
    beginInsertRows({}, tasks_.size(), tasks_.size());
    tasks_.push_back(std::move(t));
    endInsertRows();
}
```

### 18. `Q_PROPERTY` with `NOTIFY` signal that fires unconditionally

Binding loops in QML. Frame-rate drops to single digits.

```cpp
// Wrong
void setTitle(const QString& t) {
    title_ = t;
    emit titleChanged();             // fires even if t == title_
}

// Right
void setTitle(const QString& t) {
    if (t == title_) return;
    title_ = t;
    emit titleChanged();
}
```

### 19. GUI calls from a worker thread

`QPainter`, `QWidget::update()`, `QMessageBox::warning()` from a `QtConcurrent::run` callback. Crash on the worker thread, sometimes much later.

```cpp
// Wrong
QtConcurrent::run([this] {
    auto data = fetch();
    label_->setText(data);            // GUI from worker — UB
});

// Right
QtConcurrent::run([this] {
    auto data = fetch();
    QMetaObject::invokeMethod(this, [this, data] {
        label_->setText(data);
    }, Qt::QueuedConnection);
});
```

### 20. `qmlRegisterType` in new code

Qt 6 prefers `QML_ELEMENT` + `qt_add_qml_module`. Models trained on Qt 5 examples pick the old API.

```cpp
// Wrong (Qt 5 style, still compiles)
qmlRegisterType<TaskModel>("MyApp", 1, 0, "TaskModel");

// Right (Qt 6.8 style)
class TaskModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT                           // automatic registration
};
```

### 21. `setContextProperty` for new code

Same era issue. Use `QML_ELEMENT` + `engine.loadFromModule(...)`.

```cpp
// Wrong
engine.rootContext()->setContextProperty("taskModel", &model);
engine.load(QUrl("qrc:/Main.qml"));

// Right
engine.loadFromModule("MyApp", "Main");   // QML_ELEMENT-registered models autoload
```

---

## Top Failure Modes — CMake 4.0+

### 22. `cmake_minimum_required(VERSION 3.x)`

CMake 4.0 hard-fails on minimum < 3.5. Models often suggest old minimums to "be safe."

```cmake
# Wrong
cmake_minimum_required(VERSION 3.10)

# Right (this project)
cmake_minimum_required(VERSION 4.0)
```

### 23. `file(GLOB ...)` for sources

CMake re-runs only when CMakeLists changes, so adding a file means a stale build. List explicitly.

```cmake
# Wrong
file(GLOB SOURCES "src/*.cpp")
add_library(foo ${SOURCES})

# Right
add_library(foo
    src/Foo.cpp
    src/Bar.cpp)
```

### 24. Directory-scoped `include_directories`, `link_libraries`

Pollutes every target in the directory. Use target-scoped instead.

```cmake
# Wrong
include_directories(${CMAKE_SOURCE_DIR}/include)
link_libraries(Qt6::Core)

# Right
target_include_directories(foo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
target_link_libraries(foo PRIVATE Qt6::Core)
```

### 25. Missing `target_compile_features(target PUBLIC cxx_std_23)`

Project-wide `CMAKE_CXX_STANDARD` works for first-party targets but doesn't propagate to consumers. Set it on the target.

---

## How Reviewers Catch These

| Failure | Caught by |
|---|---|
| 1, 16 | `cpp-build-on-edit` hook (link error or runtime miss) |
| 2, 3, 11, 19 | `/qt-reviewer`, ASan |
| 4, 12 | `/cpp-reviewer`, `clang-tidy` |
| 5, 6, 7 | Compiler `-Wall`, ASan, UBSan |
| 8, 9, 14 | `clang-tidy` `bugprone-*` checks |
| 10 | `clang-tidy-on-edit` hook (warning) |
| 13 | `/security-reviewer`, `/hunt` skill |
| 15, 22, 23, 24, 25 | `/cpp-build-resolver`, CMake configure |
| 17, 18, 20, 21 | `/qt-reviewer`, `qmllint` |

---

## Reading Compiler Errors

A pattern that helps every model and every human:

1. **Read the first error, not the last.** Compilers cascade.
2. **In template errors, find `required from here`.** That's the user-code line.
3. **In linker errors, identify the symbol class.** Vtable means missing virtual definition; constructor means missing source in target; member means template instantiation issue.
4. **Strip the noise.** GCC error walls often have one signal line buried 200 lines deep — grep the model output for `error:` first.

When stuck, paste the **first three errors** (verbatim, including file:line) into `/cpp-build-resolver`. Do not paraphrase.

---

## Reference

- Steering: `cpp-patterns.md`, `qt-patterns.md` (auto-loaded from `~/.kiro/steering/`)
- Skills: `cpp-patterns`, `qt-patterns`
- Workspace rules: [`AGENTS.md`](../AGENTS.md)
- Playbook: [`Agents-Playbook.md`](Agents-Playbook.md)
