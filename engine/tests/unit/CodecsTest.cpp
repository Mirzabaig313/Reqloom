// Unit tests for domain/Codecs.h — base64, hex, url, methodToString.
//
// These are pure-function tests with no I/O. Each group pins RFC vectors
// or well-known values so an implementation swap surfaces immediately.
//
// Each test fails on the parent commit if the codec implementation is
// broken or the function doesn't exist.
#include "domain/Codecs.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace ce = chainapi::engine;
namespace codecs = chainapi::engine::codecs;

// ─── base64Encode ────────────────────────────────────────────────────────────

TEST(Base64Encode, empty_input_returns_empty_string) {
    EXPECT_EQ(codecs::base64Encode(""), "");
}

TEST(Base64Encode, single_byte_pads_to_four_chars) {
    // 'M' = 0x4D → "TQ=="
    EXPECT_EQ(codecs::base64Encode("M"), "TQ==");
}

TEST(Base64Encode, two_bytes_pad_to_four_chars) {
    // 'Ma' = 0x4D 0x61 → "TWE="
    EXPECT_EQ(codecs::base64Encode("Ma"), "TWE=");
}

TEST(Base64Encode, three_bytes_produce_no_padding) {
    // 'Man' = 0x4D 0x61 0x6E → "TWFu"
    EXPECT_EQ(codecs::base64Encode("Man"), "TWFu");
}

TEST(Base64Encode, rfc4648_test_vector_hello_world) {
    // RFC 4648 §10 test vector: "Hello World" → "SGVsbG8gV29ybGQ="
    EXPECT_EQ(codecs::base64Encode("Hello World"), "SGVsbG8gV29ybGQ=");
}

TEST(Base64Encode, rfc7617_basic_auth_vector) {
    // RFC 7617 §2: "Aladdin:open sesame" → "QWxhZGRpbjpvcGVuIHNlc2FtZQ=="
    EXPECT_EQ(codecs::base64Encode("Aladdin:open sesame"), "QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
}

TEST(Base64Encode, binary_data_with_all_byte_values) {
    // Encode a 3-byte sequence that exercises all 6-bit groups.
    // 0x00 0x01 0x02 → "AAEC"
    const std::string input{'\x00', '\x01', '\x02'};
    EXPECT_EQ(codecs::base64Encode(input), "AAEC");
}

TEST(Base64Encode, output_uses_only_alphabet_plus_padding) {
    // Encode 256 bytes (all possible byte values) and confirm only
    // base64 alphabet characters appear.
    std::string all256;
    all256.reserve(256);
    for (int i = 0; i < 256; ++i) all256.push_back(static_cast<char>(i));
    const auto encoded = codecs::base64Encode(all256);
    for (const char c : encoded) {
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        EXPECT_TRUE(valid) << "unexpected char: " << c;
    }
}

// ─── base64Decode ────────────────────────────────────────────────────────────

TEST(Base64Decode, empty_input_returns_empty_string) {
    auto r = codecs::base64Decode("");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
}

TEST(Base64Decode, decodes_single_byte_with_double_padding) {
    auto r = codecs::base64Decode("TQ==");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "M");
}

TEST(Base64Decode, decodes_two_bytes_with_single_padding) {
    auto r = codecs::base64Decode("TWE=");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "Ma");
}

TEST(Base64Decode, decodes_three_bytes_without_padding) {
    auto r = codecs::base64Decode("TWFu");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "Man");
}

TEST(Base64Decode, rfc7617_basic_auth_round_trip) {
    const auto encoded = codecs::base64Encode("Aladdin:open sesame");
    auto decoded = codecs::base64Decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, "Aladdin:open sesame");
}

TEST(Base64Decode, tolerates_embedded_whitespace) {
    // Whitespace (\n \r \t space) is tolerated per the implementation.
    auto r = codecs::base64Decode("TQ\n==");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "M");
}

TEST(Base64Decode, rejects_non_alphabet_character) {
    // '@' is not in the base64 alphabet.
    EXPECT_FALSE(codecs::base64Decode("TQ@=").has_value());
}

TEST(Base64Decode, rejects_padding_in_middle_of_stream) {
    // '=' before the end of the stream is invalid.
    EXPECT_FALSE(codecs::base64Decode("T=Qu").has_value());
}

TEST(Base64Decode, rejects_more_than_two_padding_chars) {
    EXPECT_FALSE(codecs::base64Decode("A===").has_value());
}

TEST(Base64Decode, rejects_single_trailing_base64_char_orphan) {
    // A single base64 char at the end (rem==1 after stripping padding) cannot
    // decode to any byte. Need 5 non-padding chars to produce rem==1.
    // "TWFuA" → buf="TWFuA", size=5, loop processes 4, rem=1 → nullopt.
    EXPECT_FALSE(codecs::base64Decode("TWFuA").has_value());
}

TEST(Base64Decode, round_trips_all_256_byte_values) {
    std::string all256;
    all256.reserve(256);
    for (int i = 0; i < 256; ++i) all256.push_back(static_cast<char>(i));
    const auto encoded = codecs::base64Encode(all256);
    auto decoded = codecs::base64Decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, all256);
}

// ─── hexEncode ───────────────────────────────────────────────────────────────

TEST(HexEncode, empty_input_returns_empty_string) {
    EXPECT_EQ(codecs::hexEncode(""), "");
}

TEST(HexEncode, single_byte_zero_encodes_to_two_zeros) {
    const std::string input{'\x00'};
    EXPECT_EQ(codecs::hexEncode(input), "00");
}

TEST(HexEncode, single_byte_ff_encodes_correctly) {
    const std::string input{'\xFF'};
    EXPECT_EQ(codecs::hexEncode(input), "ff");
}

TEST(HexEncode, output_is_lowercase) {
    // 0xAB → "ab", not "AB"
    const std::string input{'\xAB'};
    EXPECT_EQ(codecs::hexEncode(input), "ab");
}

TEST(HexEncode, known_ascii_string) {
    // "AB" = 0x41 0x42 → "4142"
    EXPECT_EQ(codecs::hexEncode("AB"), "4142");
}

TEST(HexEncode, output_length_is_double_input_length) {
    const std::string input(17, '\xDE');
    EXPECT_EQ(codecs::hexEncode(input).size(), 34u);
}

// ─── hexDecode ───────────────────────────────────────────────────────────────

TEST(HexDecode, empty_input_returns_empty_string) {
    auto r = codecs::hexDecode("");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
}

TEST(HexDecode, decodes_lowercase_hex) {
    auto r = codecs::hexDecode("4142");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "AB");
}

TEST(HexDecode, decodes_uppercase_hex) {
    auto r = codecs::hexDecode("4142");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "AB");
}

TEST(HexDecode, decodes_mixed_case) {
    // "deadBEEF" → 0xDE 0xAD 0xBE 0xEF
    auto r = codecs::hexDecode("deadBEEF");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string("\xDE\xAD\xBE\xEF", 4));
}

TEST(HexDecode, rejects_odd_length_input) {
    EXPECT_FALSE(codecs::hexDecode("abc").has_value());
    EXPECT_FALSE(codecs::hexDecode("a").has_value());
}

TEST(HexDecode, rejects_non_hex_character) {
    EXPECT_FALSE(codecs::hexDecode("GG").has_value());
    EXPECT_FALSE(codecs::hexDecode("4Z").has_value());
}

TEST(HexDecode, round_trips_all_256_byte_values) {
    std::string all256;
    all256.reserve(256);
    for (int i = 0; i < 256; ++i) all256.push_back(static_cast<char>(i));
    const auto encoded = codecs::hexEncode(all256);
    auto decoded = codecs::hexDecode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, all256);
}

// ─── urlEncode ───────────────────────────────────────────────────────────────

TEST(UrlEncode, empty_input_returns_empty_string) {
    EXPECT_EQ(codecs::urlEncode(""), "");
}

TEST(UrlEncode, unreserved_chars_pass_through_unchanged) {
    // RFC 3986 §2.3 unreserved: A-Z a-z 0-9 - _ . ~
    EXPECT_EQ(codecs::urlEncode("Az09-_.~"), "Az09-_.~");
}

TEST(UrlEncode, space_encodes_to_percent_20) {
    EXPECT_EQ(codecs::urlEncode("hello world"), "hello%20world");
}

TEST(UrlEncode, reserved_chars_are_percent_encoded) {
    // '/' → %2F, '?' → %3F, '&' → %26, '=' → %3D, '+' → %2B
    EXPECT_EQ(codecs::urlEncode("/"), "%2F");
    EXPECT_EQ(codecs::urlEncode("?"), "%3F");
    EXPECT_EQ(codecs::urlEncode("&"), "%26");
    EXPECT_EQ(codecs::urlEncode("="), "%3D");
    EXPECT_EQ(codecs::urlEncode("+"), "%2B");
}

TEST(UrlEncode, percent_encoding_uses_uppercase_hex) {
    // 0xAB → %AB, not %ab
    const std::string input{'\xAB'};
    EXPECT_EQ(codecs::urlEncode(input), "%AB");
}

TEST(UrlEncode, at_sign_is_encoded) {
    EXPECT_EQ(codecs::urlEncode("user@example.com"), "user%40example.com");
}

TEST(UrlEncode, colon_is_encoded) {
    EXPECT_EQ(codecs::urlEncode("http://x"), "http%3A%2F%2Fx");
}

// ─── urlDecode ───────────────────────────────────────────────────────────────

TEST(UrlDecode, empty_input_returns_empty_string) {
    auto r = codecs::urlDecode("");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
}

TEST(UrlDecode, plain_text_passes_through) {
    auto r = codecs::urlDecode("hello");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "hello");
}

TEST(UrlDecode, percent_20_decodes_to_space) {
    auto r = codecs::urlDecode("hello%20world");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "hello world");
}

TEST(UrlDecode, plus_decodes_to_space) {
    auto r = codecs::urlDecode("a+b");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "a b");
}

TEST(UrlDecode, mixed_plus_and_percent_encoding) {
    auto r = codecs::urlDecode("a+b%20c");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "a b c");
}

TEST(UrlDecode, decodes_lowercase_percent_escape) {
    auto r = codecs::urlDecode("%2f");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/");
}

TEST(UrlDecode, decodes_uppercase_percent_escape) {
    auto r = codecs::urlDecode("%2F");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/");
}

TEST(UrlDecode, rejects_truncated_percent_escape_at_end) {
    // '%' with only one following char — not enough for %HH.
    EXPECT_FALSE(codecs::urlDecode("%2").has_value());
    EXPECT_FALSE(codecs::urlDecode("%").has_value());
}

TEST(UrlDecode, rejects_non_hex_percent_escape) {
    EXPECT_FALSE(codecs::urlDecode("%ZZ").has_value());
    EXPECT_FALSE(codecs::urlDecode("%GG").has_value());
}

TEST(UrlDecode, round_trips_with_url_encode) {
    const std::string original = "hello world / ?&=+@#";
    auto decoded = codecs::urlDecode(codecs::urlEncode(original));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, original);
}

// ─── methodToString ──────────────────────────────────────────────────────────

TEST(MethodToString, all_seven_methods_produce_correct_strings) {
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Get), "GET");
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Post), "POST");
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Put), "PUT");
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Patch), "PATCH");
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Delete), "DELETE");
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Head), "HEAD");
    EXPECT_EQ(codecs::methodToString(ce::HttpMethod::Options), "OPTIONS");
}

TEST(MethodToString, output_is_uppercase) {
    for (const auto m : {ce::HttpMethod::Get,
                         ce::HttpMethod::Post,
                         ce::HttpMethod::Put,
                         ce::HttpMethod::Patch,
                         ce::HttpMethod::Delete,
                         ce::HttpMethod::Head,
                         ce::HttpMethod::Options}) {
        const auto s = codecs::methodToString(m);
        for (const char c : s) {
            EXPECT_TRUE(c >= 'A' && c <= 'Z') << "lowercase char in: " << s;
        }
    }
}

// ─── truncateUtf8 ────────────────────────────────────────────────────────────

TEST(TruncateUtf8, returns_input_unchanged_when_within_cap) {
    EXPECT_EQ(codecs::truncateUtf8("hello", 5), "hello");
    EXPECT_EQ(codecs::truncateUtf8("hi", 100), "hi");
}

TEST(TruncateUtf8, truncates_ascii_at_exact_byte_cap) {
    EXPECT_EQ(codecs::truncateUtf8("hello world", 5), "hello");
}

TEST(TruncateUtf8, never_exceeds_max_bytes) {
    const std::string s(1000, 'x');
    EXPECT_EQ(codecs::truncateUtf8(s, 10).size(), 10u);
}

TEST(TruncateUtf8, backs_off_rather_than_splitting_a_multibyte_char) {
    // "a€" = 0x61 (a) + 0xE2 0x82 0xAC (€, 3 bytes). Cutting at 2 bytes
    // would land inside the € sequence; the result must back off to "a"
    // so the string stays valid UTF-8.
    const std::string s = "a\xE2\x82\xAC";
    const auto out = codecs::truncateUtf8(s, 2);
    EXPECT_EQ(out, "a");
}

TEST(TruncateUtf8, keeps_a_whole_multibyte_char_when_it_fits) {
    // Cap of 4 fits "a" + the full 3-byte € → unchanged.
    const std::string s = "a\xE2\x82\xAC";
    EXPECT_EQ(codecs::truncateUtf8(s, 4), s);
}

TEST(TruncateUtf8, backs_off_from_a_four_byte_emoji_boundary) {
    // "😀" = 0xF0 0x9F 0x98 0x80 (4 bytes). Any cap of 1-3 must yield empty
    // rather than a partial sequence.
    const std::string emoji = "\xF0\x9F\x98\x80";
    EXPECT_TRUE(codecs::truncateUtf8(emoji, 3).empty());
    EXPECT_TRUE(codecs::truncateUtf8(emoji, 2).empty());
    EXPECT_TRUE(codecs::truncateUtf8(emoji, 1).empty());
    EXPECT_EQ(codecs::truncateUtf8(emoji, 4), emoji);
}

TEST(TruncateUtf8, zero_cap_returns_empty) {
    EXPECT_TRUE(codecs::truncateUtf8("anything", 0).empty());
}
