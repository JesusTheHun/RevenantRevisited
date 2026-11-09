// Included after the production reader declarations and method bodies.
namespace {
class ScrollReader : public testing::Test {
protected:
    void SetUp() override {
        ScrollPane.Close(); BookPane.Close(); PlayScreen = {};
        fonts.missing = false; TMulti::missingArchive = false; TMulti::missingPaper = false;
        textDraws.clear(); renderer.paperDraws = 0;
    }
    void TearDown() override { ScrollPane.Close(); BookPane.Close(); }
    static TScroll Content(int count, const char* type = "Scroll") {
        TScroll scroll;
        scroll.type = type;
        for (int i = 0; i < count; ++i) scroll.text += "instruction " + std::to_string(i) + "\n";
        return scroll;
    }
    static void Read(TScroll& scroll) {
        ASSERT_TRUE(scroll.Use(nullptr));
        PlayScreen.OpenNextPane();
        ASSERT_NE(PlayScreen.modal, nullptr);
    }
};

TEST_F(ScrollReader, UseDefersInitializationAndReportsHandled) {
    auto scroll = Content(1);
    EXPECT_FALSE(scroll.Use(nullptr, 3));
    EXPECT_EQ(PlayScreen.nextpane, nullptr);
    ASSERT_TRUE(scroll.Use(nullptr));
    EXPECT_FALSE(ScrollPane.IsOpen());
    EXPECT_EQ(PlayScreen.nextpane, &ScrollPane);
    PlayScreen.OpenNextPane();
    EXPECT_TRUE(ScrollPane.IsOpen());
    EXPECT_EQ(PlayScreen.modal, &ScrollPane);
    EXPECT_EQ(PlayScreen.nextpane, nullptr);
    EXPECT_EQ(PlayScreen.flags, TScreen::MODAL_GAME | TScreen::MODAL_ANIMATE);
    EXPECT_FALSE(scroll.Use(nullptr)); // no duplicate modal or reinitialization
    EXPECT_EQ(TMulti::live, 1);
}

TEST_F(ScrollReader, ComposesActualOwnedTextWithNamedFontAndPaper) {
    auto scroll = Content(2);
    ASSERT_TRUE(scroll.Use(nullptr));
    scroll.text = "changed after Use";
    PlayScreen.OpenNextPane();
    ScrollPane.Compose();
    ASSERT_EQ(textDraws.size(), 2);
    EXPECT_EQ(textDraws[0].text, "instruction 0");
    EXPECT_EQ(textDraws[0].x, 100);
    EXPECT_EQ(textDraws[0].y, 94);
    EXPECT_EQ(textDraws[1].y, 114); // FONT.DEF line spacing
    EXPECT_FLOAT_EQ(textDraws[0].r, 74 / 255.0f);
    EXPECT_EQ(fonts.requested, "Scroll");
    EXPECT_EQ(renderer.paperDraws, 1);
    EXPECT_TRUE(ScrollPane.Button(0)->hidden);
    EXPECT_TRUE(ScrollPane.Button(1)->hidden);
}

TEST_F(ScrollReader, PagingKeysAndButtonsNeverShowAnEmptyLastPage) {
    auto scroll = Content(25);
    Read(scroll);
    EXPECT_FALSE(ScrollPane.Button(0)->hidden);
    ScrollPane.KeyPress(VK_NEXT, true);
    ScrollPane.Compose();
    ASSERT_FALSE(textDraws.empty());
    EXPECT_EQ(textDraws[0].text, "instruction 11");
    textDraws.clear();
    ScrollPane.KeyPress(VK_END, true);
    ScrollPane.Compose();
    ASSERT_EQ(textDraws.size(), 11);
    EXPECT_EQ(textDraws.back().text, "instruction 24");
    EXPECT_TRUE(ScrollPane.Button(0)->hidden);
    EXPECT_FALSE(ScrollPane.Button(1)->hidden);
    ScrollPane.KeyPress(VK_HOME, true);
    EXPECT_TRUE(ScrollPane.Button(1)->hidden);
    textDraws.clear();
    ScrollPane.KeyPress(VK_DOWN, false);
    ScrollPane.KeyPress(VK_UP, true);
    ScrollPane.Compose();
    EXPECT_EQ(textDraws[0].text, "instruction 0");
}

TEST_F(ScrollReader, BookUsesTwoColumnsAndPagesToItsLastSpread) {
    auto scroll = Content(33, "Book");
    Read(scroll);
    EXPECT_EQ(PlayScreen.modal, &BookPane);
    EXPECT_EQ(fonts.requested, "Book");
    BookPane.Compose();
    ASSERT_EQ(textDraws.size(), 34); // 32 body lines + two page numbers
    EXPECT_EQ(textDraws[0].text, "1");
    EXPECT_EQ(textDraws[17].text, "2");
    EXPECT_EQ(textDraws[1].x, 101);
    EXPECT_EQ(textDraws[18].x, 333);
    BookPane.KeyPress(VK_NEXT, true);
    textDraws.clear();
    BookPane.Compose();
    ASSERT_EQ(textDraws.size(), 3);
    EXPECT_EQ(textDraws[0].text, "3");
    EXPECT_EQ(textDraws[1].text, "instruction 32");
    EXPECT_EQ(textDraws[2].text, "4");
    EXPECT_TRUE(BookPane.Button(0)->hidden);
}

TEST_F(ScrollReader, CanonicalWordWrappingDeterminesPagination) {
    TScroll scroll;
    for (int i = 0; i < 90; ++i) scroll.text += "alpha ";
    Read(scroll);
    ScrollPane.Compose();
    ASSERT_EQ(textDraws.size(), 11);
    EXPECT_EQ(textDraws[0].text, "alpha alpha alpha alpha alpha alpha alpha");
    EXPECT_FALSE(ScrollPane.Button(0)->hidden);
    ScrollPane.KeyPress(VK_END, true);
    textDraws.clear();
    ScrollPane.Compose();
    EXPECT_FALSE(textDraws.empty());
    EXPECT_TRUE(ScrollPane.Button(0)->hidden);
    EXPECT_TRUE(std::all_of(textDraws.begin(), textDraws.end(), [](const auto& draw) {
        return !draw.text.empty() && draw.text.size() <= 43;
    }));
}

TEST_F(ScrollReader, ExitDefersCleanupAndReaderCanReopen) {
    auto scroll = Content(25);
    Read(scroll);
    ScrollPane.KeyPress(VK_NEXT, true);
    ScrollPane.KeyPress(VK_ESCAPE, true);
    EXPECT_EQ(ScrollPane.ends, 1);
    EXPECT_TRUE(ScrollPane.IsOpen());
    EXPECT_EQ(ScrollPane.buttons.size(), 3); // not deleted in the button callback
    PlayScreen.Finish();
    EXPECT_FALSE(ScrollPane.IsOpen());
    EXPECT_EQ(TMulti::live, 0);
    EXPECT_EQ(PlayScreen.redraws, 1);
    Read(scroll);
    ScrollPane.Compose();
    EXPECT_EQ(textDraws[0].text, "instruction 0");
    ScrollPane.OnScreenEvent(TScreen::SCREENEVENT_CLOSING, nullptr);
    EXPECT_FALSE(ScrollPane.IsOpen());
    EXPECT_EQ(TMulti::live, 0);
}

TEST_F(ScrollReader, FailedInitializationAndModalPushReleaseResources) {
    auto scroll = Content(1);
    for (int failure = 0; failure < 4; ++failure) {
        TMulti::missingArchive = failure == 0;
        TMulti::missingPaper = failure == 1;
        fonts.missing = failure == 2;
        PlayScreen.rejectPush = failure == 3;
        ASSERT_TRUE(scroll.Use(nullptr));
        PlayScreen.OpenNextPane();
        EXPECT_FALSE(ScrollPane.IsOpen());
        EXPECT_EQ(PlayScreen.nextpane, nullptr);
        EXPECT_EQ(PlayScreen.modal, nullptr);
        EXPECT_EQ(TMulti::live, 0);
    }
}

TEST_F(ScrollReader, ResizeKeepsClassicSizeAndMatchingPaneOrigin) {
    auto scroll = Content(1);
    Read(scroll);
    EXPECT_EQ(ScrollPane.x, 640);
    EXPECT_EQ(ScrollPane.y, 300);
    ScrollPane.OnCanvasResize(854, 480);
    EXPECT_EQ(ScrollPane.x, 107);
    EXPECT_EQ(ScrollPane.y, 0);
    EXPECT_EQ(ScrollPane.width, 640);
    EXPECT_EQ(ScrollPane.height, 480);
}
} // namespace
