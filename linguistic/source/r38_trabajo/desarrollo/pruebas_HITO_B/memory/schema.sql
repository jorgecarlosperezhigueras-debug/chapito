-- kotoGBA: extensión de la base DI03_ES existente, esquema 1.
-- Las tablas entries/forms/metadata conservan su contrato original.
PRAGMA foreign_keys = ON;
CREATE TABLE koto_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE koto_sources(source_id TEXT PRIMARY KEY, kind TEXT NOT NULL,
    sha256 TEXT NOT NULL, path_hint TEXT NOT NULL, provenance_json TEXT NOT NULL);
CREATE TABLE koto_games(game_id TEXT PRIMARY KEY, edition TEXT NOT NULL,
    base_sha256 TEXT NOT NULL, base_payload BLOB NOT NULL, capacity INTEGER NOT NULL,
    revision INTEGER NOT NULL DEFAULT 0, runtime_revision INTEGER NOT NULL DEFAULT 0,
    provenance_json TEXT NOT NULL);
CREATE TABLE koto_documents(document_id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES koto_games(game_id), source_offset INTEGER NOT NULL,
    source_end INTEGER NOT NULL, source_sha256 TEXT NOT NULL, original_bytes BLOB NOT NULL,
    raw_source BLOB NOT NULL, original_kana TEXT, japanese TEXT NOT NULL, spanish TEXT NOT NULL,
    metadata_json TEXT NOT NULL, fragments_json TEXT NOT NULL, legacy_words_json TEXT NOT NULL,
    UNIQUE(game_id,source_offset));
CREATE TABLE koto_lexemes(lexeme_id TEXT PRIMARY KEY, kind TEXT NOT NULL,
    lemma TEXT NOT NULL, reading TEXT NOT NULL, provenance_json TEXT NOT NULL);
CREATE TABLE koto_senses(sense_id TEXT PRIMARY KEY,
    lexeme_id TEXT NOT NULL REFERENCES koto_lexemes(lexeme_id), meaning_es TEXT NOT NULL,
    provenance_json TEXT NOT NULL);
CREATE TABLE koto_card_variants(variant_id TEXT PRIMARY KEY, runtime_identity INTEGER NOT NULL,
    payload_sha256 TEXT NOT NULL, lexeme_id TEXT NOT NULL REFERENCES koto_lexemes(lexeme_id),
    sense_id TEXT NOT NULL REFERENCES koto_senses(sense_id), card_json TEXT NOT NULL,
    provenance_json TEXT NOT NULL, UNIQUE(runtime_identity,payload_sha256));
CREATE INDEX koto_card_runtime ON koto_card_variants(runtime_identity);
CREATE TABLE koto_new_identities(runtime_identity INTEGER PRIMARY KEY,
    payload_sha256 TEXT NOT NULL UNIQUE, payload_json TEXT NOT NULL);
CREATE TABLE koto_document_cards(document_id TEXT NOT NULL REFERENCES koto_documents(document_id),
    ordinal INTEGER NOT NULL, variant_id TEXT NOT NULL REFERENCES koto_card_variants(variant_id),
    PRIMARY KEY(document_id,ordinal));
CREATE TABLE koto_units(document_id TEXT NOT NULL REFERENCES koto_documents(document_id),
    ordinal INTEGER NOT NULL, raw_start INTEGER NOT NULL, raw_end INTEGER NOT NULL,
    display_start INTEGER NOT NULL, display_end INTEGER NOT NULL,
    display TEXT NOT NULL, reading TEXT NOT NULL, PRIMARY KEY(document_id,ordinal));
CREATE TABLE koto_occurrences(occurrence_id TEXT PRIMARY KEY,
    document_id TEXT NOT NULL REFERENCES koto_documents(document_id), ordinal INTEGER NOT NULL,
    raw_start INTEGER NOT NULL, raw_end INTEGER NOT NULL,
    display_start INTEGER NOT NULL, display_end INTEGER NOT NULL, flags INTEGER NOT NULL,
    baseline_variant_id TEXT NOT NULL REFERENCES koto_card_variants(variant_id),
    variant_id TEXT NOT NULL REFERENCES koto_card_variants(variant_id),
    revision INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL CHECK(status IN ('generated','reviewed','rejected','pending')),
    reviewer TEXT NOT NULL, reason TEXT NOT NULL, provenance_json TEXT NOT NULL,
    active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),
    UNIQUE(document_id,ordinal), UNIQUE(document_id,display_start,display_end));
CREATE INDEX koto_occurrence_variant ON koto_occurrences(variant_id);
CREATE TABLE koto_teacher_notes(note_id TEXT PRIMARY KEY,
    document_id TEXT NOT NULL REFERENCES koto_documents(document_id), ordinal INTEGER NOT NULL,
    pattern_id TEXT, body_es TEXT NOT NULL, anchor_jp TEXT NOT NULL,
    runtime_json TEXT NOT NULL, provenance_json TEXT NOT NULL,
    UNIQUE(document_id,ordinal));
CREATE TABLE koto_grammar_patterns(pattern_id TEXT PRIMARY KEY,
    payload_json TEXT NOT NULL, provenance_json TEXT NOT NULL);
CREATE TABLE koto_review_reasons(reason_id TEXT PRIMARY KEY,
    occurrence_id TEXT REFERENCES koto_occurrences(occurrence_id),
    document_id TEXT NOT NULL REFERENCES koto_documents(document_id), code TEXT NOT NULL,
    details_json TEXT NOT NULL);
CREATE INDEX koto_review_code ON koto_review_reasons(code);
CREATE TABLE koto_kanji(kanji_id TEXT PRIMARY KEY, character TEXT NOT NULL UNIQUE,
    k20_ordinal INTEGER, readings_json TEXT NOT NULL, meanings_es_json TEXT NOT NULL,
    components_json TEXT NOT NULL, status TEXT NOT NULL
    CHECK(status IN ('generated','reviewed','rejected','pending')),
    provenance_json TEXT NOT NULL);
CREATE TABLE koto_imports(packet_id TEXT PRIMARY KEY, packet_sha256 TEXT NOT NULL,
    game_id TEXT NOT NULL REFERENCES koto_games(game_id), result_json TEXT NOT NULL);
CREATE TABLE koto_packets(packet_id TEXT PRIMARY KEY REFERENCES koto_imports(packet_id),
    packet_json TEXT NOT NULL);
CREATE TABLE koto_changes(change_id INTEGER PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES koto_games(game_id), revision INTEGER NOT NULL,
    packet_id TEXT NOT NULL REFERENCES koto_imports(packet_id), occurrence_id TEXT NOT NULL
    REFERENCES koto_occurrences(occurrence_id), before_json TEXT NOT NULL, after_json TEXT NOT NULL);
-- No se abren ni importan archivos de colecciones personales.
