#include "common/screen.hpp"

#include <vterm.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ccb {

class Screen::Impl {
public:
    Impl(int cols, int rows, Reply reply) : reply_(std::move(reply)) {
        vt_ = vterm_new(rows, cols);
        if (!vt_) throw std::runtime_error("cannot create terminal screen");
        vterm_set_utf8(vt_, 1);
        vterm_output_set_callback(vt_, &Impl::forward_reply, this);
        screen_ = vterm_obtain_screen(vt_);
        vterm_screen_enable_altscreen(screen_, 1);
        vterm_screen_reset(screen_, 1);
    }

    ~Impl() { vterm_free(vt_); }

    void feed(std::string_view bytes) {
        if (!bytes.empty()) vterm_input_write(vt_, bytes.data(), bytes.size());
    }

    void resize(int cols, int rows) {
        if (cols > 0 && rows > 0) vterm_set_size(vt_, rows, cols);
    }

    std::string text() const {
        int rows = 0;
        int cols = 0;
        vterm_get_size(vt_, &rows, &cols);
        std::string joined;
        std::vector<char> row_buffer;
        for (int row = 0; row < rows; ++row) {
            VTermRect rect{row, row + 1, 0, cols};
            size_t needed = vterm_screen_get_text(screen_, nullptr, 0, rect);
            row_buffer.assign(needed, '\0');
            size_t written = needed ? vterm_screen_get_text(screen_, row_buffer.data(), row_buffer.size(), rect) : 0;
            std::string line(row_buffer.data(), std::min(written, row_buffer.size()));
            while (!line.empty() && (line.back() == ' ' || line.back() == '\n')) line.pop_back();
            if (row) joined.push_back('\n');
            joined += line;
        }
        return joined;
    }

private:
    static void forward_reply(const char* bytes, size_t length, void* self) {
        auto* impl = static_cast<Impl*>(self);
        if (impl->reply_) impl->reply_(std::string_view(bytes, length));
    }

    Reply reply_;
    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
};

Screen::Screen(int cols, int rows, Reply reply) : impl_(std::make_unique<Impl>(cols, rows, std::move(reply))) {}

Screen::~Screen() = default;

void Screen::feed(std::string_view bytes) { impl_->feed(bytes); }

void Screen::resize(int cols, int rows) { impl_->resize(cols, rows); }

std::string Screen::text() const { return impl_->text(); }

}
