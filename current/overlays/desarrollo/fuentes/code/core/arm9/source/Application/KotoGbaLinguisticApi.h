#pragma once

// KL29 v1. All offsets accepted by this API are ROM FILE offsets (not 0x08
// guest pointers). Returned strings belong to the current, verified document.
// A consumer retaining a string across reader slices MUST hold a reference pin.
// Request only changes a target. Only LinguisticSlice submits asynchronous I/O.
enum {
    // Hito A: retain every historical card while correcting the largest document.
    LINGUISTIC_DOCUMENT_BYTES = 18944,
    LINGUISTIC_R39_DOCUMENT_BYTES = 18432,
    LINGUISTIC_LEGACY_DOCUMENT_BYTES = 16384,
    LINGUISTIC_MAX_UNITS = 192,
    LINGUISTIC_MAX_NOTES = 6,
    LINGUISTIC_COMPOSITION_REQUIRED = 1,
    LINGUISTIC_DEPENDENCIES_REQUIRED = 2,
    LINGUISTIC_SOURCE_COMPLETE_REQUIRED = 4
};

struct LinguisticUnit {
    u16 rawStart, rawEnd;
    const char* display;
    const char* reading;
    u16 displayBytes, readingBytes;
};

struct LinguisticNote {
    u32 id;
    u16 rawStart, rawEnd, displayStart, displayEnd;
    const char* body;
    const char* anchor;
    u16 bodyBytes, anchorBytes;
    u32 requiredSources[2];
    u16 flags, bodyLines;
};

struct LinguisticFragment {
    u16 rawStart, rawEnd;
    const char* spanish;
    u16 spanishBytes;
};
struct LinguisticWord {u16 rawStart,rawEnd,card,ref;};
// R32 optional source-bound cards. Display spans are exact UTF8 byte offsets
// into the concatenation of the unchanged, verified unit.display strings.
struct LinguisticInlineSpan {u16 rawStart,rawEnd,displayStart,displayEnd,card,flags;};
struct LinguisticInlineCard {
    u32 identity;
    const char *surface,*reading,*meaning,*explanation;
    u16 surfaceBytes,readingBytes,meaningBytes,explanationBytes,type;
};

extern "C" void kotogba_loadLinguistic();
static bool LinguisticAvailable();
static void LinguisticRequest(u32 sourceOffset);
static bool LinguisticReady(u32 sourceOffset);
static u32 LinguisticGeneration();
static u32 LinguisticSourceOffset();
static u32 LinguisticPublicationMask();
static bool LinguisticPin(u32 sourceOffset);
static void LinguisticUnpin();
static const u8* LinguisticRaw(unsigned& bytes);
static unsigned LinguisticUnitCount();
static bool LinguisticUnitAt(unsigned index, LinguisticUnit& unit);
static unsigned LinguisticNoteCount();
static bool LinguisticNoteAt(unsigned index, LinguisticNote& note);
static unsigned LinguisticFragmentCount();
static bool LinguisticFragmentAt(unsigned index, LinguisticFragment& fragment);
static const char* LinguisticSpanish(unsigned& bytes);
static unsigned LinguisticWordCount();
static bool LinguisticWordAt(unsigned index,LinguisticWord& word);
static bool LinguisticWordsComplete();
static unsigned LinguisticInlineSpanCount();
static bool LinguisticInlineSpanAt(unsigned index,LinguisticInlineSpan& span);
static unsigned LinguisticInlineCardCount();
static bool LinguisticInlineCardAt(unsigned index,LinguisticInlineCard& card);
static void LinguisticSlice(unsigned start);

// On-disk ABI, all little endian. Header sector begins with <8s14I>:
// KOTOL029,version,totalBytes,sourceCount,noteCount,bucketCount,rootSectors,
// addressBase,dataBase,maxDocumentBytes,KS5_FNV,KC15_FNV,KM7_FNV,
// provenanceSector,provenanceBytes. rootSectors u32 roots follow at sector 1;
// each root identifies a bucket of 32 <4I> address entries:
// sourceStart,sourceEnd,documentSector,documentBytes.
//
// Document header64 <8s14I>:
// KOTOD029,source,totalBytes,bodyFNV,unitsOffset,unitCount,notesOffset,noteCount,
// stringsOffset,rawLength,fragmentsOffset,fragmentCount,spanishOffset,
// spanishBytes,flags. bodyFNV covers [64,totalBytes). SHA256 raw is [64,96),
// raw source starts at 96. All string offsets are relative to the document.
// Unit12 <6H>: rawStart,rawEnd,displayOffset,displayBytes,readingOffset,readingBytes.
// Note32: u32 id;8*u16 rawStart,rawEnd,displayStart,displayEnd,bodyOffset,
// bodyBytes,anchorOffset,anchorBytes;2*u32 requires;u16 flags,reserved.
// Fragment16 <4I>: rawStart,rawEnd,spanishOffset,spanishBytes.
// Nonempty strings have exact UTF8 byte length and one following NUL. The pair
// (offset=0,length=0) denotes an empty string; API getters return canonical "".
// Optional document.flags: bit0=word table; bit1=all applicable legacy static
// links proven; bits8..23=word table offset. Table: u16 count,u16 reserved,
// count*<4H> rawStart,rawEnd,card,ref. A source with no legacy links is complete.
// R32 bit2 adds a source-local card extension immediately after that word table:
// Header16 <8s4H>: KOTOV032,spanCount,cardCount,flags0,reserved0.
// Span12 <6H>: rawStart,rawEnd,displayByteStart,displayByteEnd,cardIndex,flags.
// Span flags:1 frozen variable role;2 reviewed exact-display override of a
// historically misbound legacy popup. Unknown bits are rejected.
// flags bit0 identifies a frozen variable role. Display offsets count UTF8
// bytes in the concatenation of all unchanged Unit.display strings.
// Card24 <I10H>: canonicalIdentity,surfaceOffset/Bytes,readingOffset/Bytes,
// meaningOffset/Bytes,explanationOffset/Bytes,type,reserved0. Types0..4:
// lexical,particle,construction,proper name,variable role. All strings remain
// in the verified document pool. Existing KS5 references and note guards keep
// their original ABI and priority; the extension does not consume KS5 RAM.
