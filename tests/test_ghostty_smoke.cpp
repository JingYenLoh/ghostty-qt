#include <QByteArray>
#include <QTest>

#include <ghostty/vt.h>

#include <array>
#include <cstring>

namespace {

struct EffectCapture {
    QByteArray writes;
    int deviceAttributeRequests = 0;
    bool receivedExpectedUserdata = true;
};

void capturePtyWrite(GhosttyTerminal, void *userdata,
                     const uint8_t *data, size_t length)
{
    auto *capture = static_cast<EffectCapture *>(userdata);
    if (capture == nullptr) {
        return;
    }
    capture->receivedExpectedUserdata = capture->receivedExpectedUserdata
        && userdata == capture;
    capture->writes.append(reinterpret_cast<const char *>(data),
                           static_cast<qsizetype>(length));
}

bool provideDeviceAttributes(GhosttyTerminal, void *userdata,
                             GhosttyDeviceAttributes *attributes)
{
    auto *capture = static_cast<EffectCapture *>(userdata);
    if (capture == nullptr || attributes == nullptr) {
        return false;
    }

    ++capture->deviceAttributeRequests;
    capture->receivedExpectedUserdata = capture->receivedExpectedUserdata
        && userdata == capture;
    *attributes = GhosttyDeviceAttributes{};
    attributes->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT420;
    attributes->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    attributes->primary.features[1] = GHOSTTY_DA_FEATURE_CLIPBOARD;
    attributes->primary.num_features = 2;
    attributes->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT420;
    attributes->secondary.firmware_version = 100;
    return true;
}

GhosttyCellWide currentCellWidth(GhosttyRenderStateRowCells cells)
{
    GhosttyCell rawCell = 0;
    GhosttyCellWide width = GHOSTTY_CELL_WIDE_NARROW;
    if (ghostty_render_state_row_cells_get(
            cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &rawCell)
            != GHOSTTY_SUCCESS
        || ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &width)
            != GHOSTTY_SUCCESS) {
        return GHOSTTY_CELL_WIDE_MAX_VALUE;
    }
    return width;
}

QByteArray currentCellText(GhosttyRenderStateRowCells cells)
{
    std::array<uint8_t, 16> storage{};
    GhosttyBuffer buffer{.ptr = storage.data(), .cap = storage.size(), .len = 0};
    if (ghostty_render_state_row_cells_get(
            cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer)
        != GHOSTTY_SUCCESS) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(storage.data()),
                      static_cast<qsizetype>(buffer.len));
}

} // namespace

class GhosttySmokeTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsesAndRendersText();
    void exposesWideCjkCellMetadata();
    void encodesInputAndPasteSafety();
    void encodesButtonDragMouseReporting();
    void answersTerminalQueriesThroughWriteCallback();
};

void GhosttySmokeTest::parsesAndRendersText()
{
    GhosttyTerminal terminal = nullptr;
    QCOMPARE(ghostty_terminal_new(nullptr, &terminal, 10, 2), GHOSTTY_SUCCESS);
    QVERIFY(terminal != nullptr);

    const char content[] = "\x1b[31mhello\x1b[0m";
    ghostty_terminal_vt_write(terminal,
                              reinterpret_cast<const uint8_t *>(content),
                              std::strlen(content));

    GhosttyRenderState state = nullptr;
    GhosttyRenderStateRowIterator rows = nullptr;
    GhosttyRenderStateRowCells cells = nullptr;
    QCOMPARE(ghostty_render_state_new(nullptr, &state), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_row_iterator_new(nullptr, &rows), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_row_cells_new(nullptr, &cells), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_update(state, terminal), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows),
             GHOSTTY_SUCCESS);
    QVERIFY(ghostty_render_state_row_iterator_next(rows));
    QCOMPARE(ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells),
             GHOSTTY_SUCCESS);

    QByteArray text;
    for (int column = 0; column < 5; ++column) {
        QVERIFY(ghostty_render_state_row_cells_next(cells));
        uint8_t storage[16]{};
        GhosttyBuffer buffer{.ptr = storage, .cap = sizeof(storage), .len = 0};
        QCOMPARE(ghostty_render_state_row_cells_get(
                     cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer),
                 GHOSTTY_SUCCESS);
        text.append(reinterpret_cast<const char *>(storage),
                    static_cast<qsizetype>(buffer.len));
    }
    QCOMPARE(text, QByteArray("hello"));

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(rows);
    ghostty_render_state_free(state);
    ghostty_terminal_free(terminal);
}

void GhosttySmokeTest::exposesWideCjkCellMetadata()
{
    GhosttyTerminal terminal = nullptr;
    QCOMPARE(ghostty_terminal_new(nullptr, &terminal, 6, 1), GHOSTTY_SUCCESS);

    // U+754C (界) has an East Asian width of two cells, followed by ASCII x.
    const QByteArray content = QByteArray::fromHex("e7958c78");
    ghostty_terminal_vt_write(terminal,
                              reinterpret_cast<const uint8_t *>(content.constData()),
                              static_cast<size_t>(content.size()));

    GhosttyRenderState state = nullptr;
    GhosttyRenderStateRowIterator rows = nullptr;
    GhosttyRenderStateRowCells cells = nullptr;
    QCOMPARE(ghostty_render_state_new(nullptr, &state), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_row_iterator_new(nullptr, &rows), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_row_cells_new(nullptr, &cells), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_update(state, terminal), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows),
             GHOSTTY_SUCCESS);
    QVERIFY(ghostty_render_state_row_iterator_next(rows));
    QCOMPARE(ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells),
             GHOSTTY_SUCCESS);

    QVERIFY(ghostty_render_state_row_cells_next(cells));
    QCOMPARE(currentCellWidth(cells), GHOSTTY_CELL_WIDE_WIDE);
    QCOMPARE(currentCellText(cells), QByteArray::fromHex("e7958c"));

    QVERIFY(ghostty_render_state_row_cells_next(cells));
    QCOMPARE(currentCellWidth(cells), GHOSTTY_CELL_WIDE_SPACER_TAIL);
    QVERIFY(currentCellText(cells).isEmpty());

    QVERIFY(ghostty_render_state_row_cells_next(cells));
    QCOMPARE(currentCellWidth(cells), GHOSTTY_CELL_WIDE_NARROW);
    QCOMPARE(currentCellText(cells), QByteArray("x"));

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(rows);
    ghostty_render_state_free(state);
    ghostty_terminal_free(terminal);
}

void GhosttySmokeTest::encodesInputAndPasteSafety()
{
    QVERIFY(ghostty_paste_is_safe("hello", 5));
    QVERIFY(!ghostty_paste_is_safe("echo one\necho two", 17));
    const QByteArray fenceInjection("hello\x1b[201~world");
    QVERIFY(!ghostty_paste_is_safe(fenceInjection.constData(),
                                   static_cast<size_t>(fenceInjection.size())));

    GhosttyTerminal terminal = nullptr;
    QCOMPARE(ghostty_terminal_new(nullptr, &terminal, 10, 2), GHOSTTY_SUCCESS);
    const char enableBracketedPaste[] = "\x1b[?2004h";
    ghostty_terminal_vt_write(
        terminal, reinterpret_cast<const uint8_t *>(enableBracketedPaste),
        sizeof(enableBracketedPaste) - 1);

    GhosttyTerminalModeConfig bracketed{
        .mode = GHOSTTY_MODE_BRACKETED_PASTE,
        .value = false,
    };
    QCOMPARE(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE,
                                  &bracketed),
             GHOSTTY_SUCCESS);
    QVERIFY(bracketed.value);

    QByteArray pasteInput("line one\nline two");
    size_t pasteSize = 0;
    QCOMPARE(ghostty_paste_encode(pasteInput.data(),
                                  static_cast<size_t>(pasteInput.size()), bracketed.value,
                                  nullptr, 0, &pasteSize),
             GHOSTTY_OUT_OF_SPACE);
    QByteArray pasteOutput(static_cast<qsizetype>(pasteSize), Qt::Uninitialized);
    size_t pasteWritten = 0;
    QCOMPARE(ghostty_paste_encode(
                 pasteInput.data(), static_cast<size_t>(pasteInput.size()), bracketed.value,
                 pasteOutput.data(), static_cast<size_t>(pasteOutput.size()), &pasteWritten),
             GHOSTTY_SUCCESS);
    pasteOutput.resize(static_cast<qsizetype>(pasteWritten));
    QCOMPARE(pasteOutput, QByteArray("\x1b[200~line one\nline two\x1b[201~"));
    ghostty_terminal_free(terminal);

    GhosttyKeyEncoder encoder = nullptr;
    GhosttyKeyEvent event = nullptr;
    QCOMPARE(ghostty_key_encoder_new(nullptr, &encoder), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_key_event_new(nullptr, &event), GHOSTTY_SUCCESS);
    ghostty_key_event_set_action(event, GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(event, GHOSTTY_KEY_C);
    ghostty_key_event_set_mods(event, GHOSTTY_MODS_CTRL);

    char output[32]{};
    size_t written = 0;
    QCOMPARE(ghostty_key_encoder_encode(encoder, event, output, sizeof(output), &written),
             GHOSTTY_SUCCESS);
    QCOMPARE(written, size_t{1});
    QCOMPARE(static_cast<unsigned char>(output[0]), static_cast<unsigned char>(0x03));

    ghostty_key_event_free(event);
    ghostty_key_encoder_free(encoder);
}

void GhosttySmokeTest::answersTerminalQueriesThroughWriteCallback()
{
    GhosttyTerminal terminal = nullptr;
    QCOMPARE(ghostty_terminal_new(nullptr, &terminal, 80, 24), GHOSTTY_SUCCESS);

    EffectCapture capture;
    QCOMPARE(ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_USERDATA, &capture),
             GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_terminal_set(
                 terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                 reinterpret_cast<const void *>(&capturePtyWrite)),
             GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_terminal_set(
                 terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
                 reinterpret_cast<const void *>(&provideDeviceAttributes)),
             GHOSTTY_SUCCESS);

    const char queries[] = "\x1b[6n\x1b[c\x1b[>c";
    ghostty_terminal_vt_write(terminal,
                              reinterpret_cast<const uint8_t *>(queries),
                              sizeof(queries) - 1);

    QCOMPARE(capture.writes,
             QByteArray("\x1b[1;1R\x1b[?64;22;52c\x1b[>41;100;0c"));
    QCOMPARE(capture.deviceAttributeRequests, 2);
    QVERIFY(capture.receivedExpectedUserdata);

    ghostty_terminal_free(terminal);
}

void GhosttySmokeTest::encodesButtonDragMouseReporting()
{
    GhosttyTerminal terminal = nullptr;
    QCOMPARE(ghostty_terminal_new(nullptr, &terminal, 80, 24), GHOSTTY_SUCCESS);
    const char enable[] = "\x1b[?1002;1006h";
    ghostty_terminal_vt_write(terminal,
                              reinterpret_cast<const uint8_t *>(enable),
                              sizeof(enable) - 1);

    GhosttyMouseEncoder encoder = nullptr;
    GhosttyMouseEvent event = nullptr;
    QCOMPARE(ghostty_mouse_encoder_new(nullptr, &encoder), GHOSTTY_SUCCESS);
    QCOMPARE(ghostty_mouse_event_new(nullptr, &event), GHOSTTY_SUCCESS);
    ghostty_mouse_encoder_setopt_from_terminal(encoder, terminal);
    const GhosttyMouseEncoderSize size{
        .size = sizeof(GhosttyMouseEncoderSize),
        .screen_width = 800,
        .screen_height = 384,
        .cell_width = 10,
        .cell_height = 16,
        .padding_top = 0,
        .padding_bottom = 0,
        .padding_right = 0,
        .padding_left = 0,
    };
    const bool anyButtonPressed = true;
    ghostty_mouse_encoder_setopt(encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    ghostty_mouse_encoder_setopt(
        encoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyButtonPressed);

    ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_MOTION);
    ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_LEFT);
    ghostty_mouse_event_set_position(event, GhosttyMousePosition{15.0F, 8.0F});
    std::array<char, 64> output{};
    size_t written = 0;
    QCOMPARE(ghostty_mouse_encoder_encode(
                 encoder, event, output.data(), output.size(), &written),
             GHOSTTY_SUCCESS);
    QCOMPARE(QByteArray(output.data(), static_cast<qsizetype>(written)),
             QByteArray("\x1b[<32;2;1M"));

    ghostty_mouse_event_free(event);
    ghostty_mouse_encoder_free(encoder);
    ghostty_terminal_free(terminal);
}

QTEST_APPLESS_MAIN(GhosttySmokeTest)

#include "test_ghostty_smoke.moc"
