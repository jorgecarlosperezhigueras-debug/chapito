// R29 reader projection. Included after ReaderFrame and before NativeScene.
static bool OrthographySourceReady(u32 source);
static u32 OrthographyGeneration();
static void OrthographyRequestFrame(const ReaderFrame& frame);
static bool OrthographyPrepare(const ReaderFrame& frame);
static bool OrthographyActive(const ReaderFrame& frame);
static u32 OrthographyCodepoint(u8 byte);
static bool OrthographyQuestion(const ReaderFrame& frame);
static unsigned OrthographyHeight(const ReaderFrame& frame,unsigned height,bool translated);
static void OrthographyRenderBegin();
static bool OrthographyRenderUnit();
static bool OrthographyTouch(unsigned x,unsigned y,u32& card,u16& ref);
static u32 OrthographyGlyphAt(unsigned x,unsigned y);
static void OrthographyAbort();
static unsigned OrthographyFullSpanish(const ReaderFrame& frame,const char* text,unsigned out);
static void CorpusOrthographyLinks(const ReaderFrame& frame);
static bool CorpusRawWord(unsigned begin,unsigned end,u32& card,u16& ref,u32 identity=0,bool restrictPartial=false);
static bool CorpusOrthographyRoute(unsigned slot,u32 guestSource);
static bool CorpusSentenceProven(unsigned slot,bool requireTranslation);
