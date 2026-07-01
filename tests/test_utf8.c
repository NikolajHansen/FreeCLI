#include <check.h>
#include <stddef.h>
#include "utf8.h"

/* -----------------------------------------------------------------------
 * utf8_clen
 * --------------------------------------------------------------------- */

START_TEST(test_clen_ascii)
{
    ck_assert_int_eq(utf8_clen("A"), 1);
    ck_assert_int_eq(utf8_clen("z"), 1);
    ck_assert_int_eq(utf8_clen(" "), 1);
}
END_TEST

START_TEST(test_clen_two_byte)
{
    /* U+00E9 é — 0xC3 0xA9 */
    ck_assert_int_eq(utf8_clen("\xC3\xA9"), 2);
}
END_TEST

START_TEST(test_clen_three_byte)
{
    /* U+4E2D 中 — 0xE4 0xB8 0xAD */
    ck_assert_int_eq(utf8_clen("\xE4\xB8\xAD"), 3);
}
END_TEST

START_TEST(test_clen_four_byte)
{
    /* U+1F600 😀 — 0xF0 0x9F 0x98 0x80 */
    ck_assert_int_eq(utf8_clen("\xF0\x9F\x98\x80"), 4);
}
END_TEST

START_TEST(test_clen_nul)
{
    ck_assert_int_eq(utf8_clen(""), 1);
}
END_TEST

/* -----------------------------------------------------------------------
 * utf8_cwidth
 * --------------------------------------------------------------------- */

START_TEST(test_cwidth_ascii)
{
    ck_assert_int_eq(utf8_cwidth("A"), 1);
    ck_assert_int_eq(utf8_cwidth(" "), 1);
}
END_TEST

START_TEST(test_cwidth_latin_ext)
{
    /* U+00E9 é — narrow (width 1) */
    ck_assert_int_eq(utf8_cwidth("\xC3\xA9"), 1);
}
END_TEST

START_TEST(test_cwidth_cjk)
{
    /* U+4E2D 中 — double-width */
    ck_assert_int_eq(utf8_cwidth("\xE4\xB8\xAD"), 2);
}
END_TEST

START_TEST(test_cwidth_emoji)
{
    /* U+1F600 😀 — treated as double-width */
    ck_assert_int_eq(utf8_cwidth("\xF0\x9F\x98\x80"), 2);
}
END_TEST

/* -----------------------------------------------------------------------
 * wrap_line_end
 * --------------------------------------------------------------------- */

START_TEST(test_wrap_fits_exactly)
{
    /* "hello" is 5 chars, width 5 — all fits, next is NUL */
    const char *next = NULL;
    const char *text = "hello";
    const char *end  = wrap_line_end(text, 0, 5, &next);
    ck_assert_ptr_eq(end, text + 5);
    ck_assert_ptr_eq(next, text + 5);
}
END_TEST

START_TEST(test_wrap_breaks_at_space)
{
    /* "hello world" in width 8 — breaks after "hello", next = "world" */
    const char *text = "hello world";
    const char *next = NULL;
    const char *end  = wrap_line_end(text, 0, 8, &next);
    /* end should point at the space */
    ck_assert_ptr_eq(end, text + 5);
    /* next skips the space */
    ck_assert_ptr_eq(next, text + 6);
}
END_TEST

START_TEST(test_wrap_newline_terminates)
{
    /* wrap_line_end stops at '\n' */
    const char *text = "hello\nworld";
    const char *next = NULL;
    const char *end  = wrap_line_end(text, 0, 40, &next);
    ck_assert_ptr_eq(end, text + 5);   /* points at '\n' */
    ck_assert_ptr_eq(next, text + 6);  /* skips '\n' */
}
END_TEST

START_TEST(test_wrap_no_space_hard_break)
{
    /* "hello" in width 3 — no space to break at, hard break at 3 */
    const char *text = "hello";
    const char *next = NULL;
    const char *end  = wrap_line_end(text, 0, 3, &next);
    ck_assert_ptr_eq(end, text + 3);
    ck_assert_ptr_eq(next, text + 3);
}
END_TEST

/* -----------------------------------------------------------------------
 * count_rows
 * --------------------------------------------------------------------- */

START_TEST(test_count_rows_single)
{
    /* Short string — fits on one row */
    ck_assert_int_eq(count_rows("hello", 0, 0, 40), 1);
}
END_TEST

START_TEST(test_count_rows_wraps)
{
    /* "hello world" in width 8 — wraps to 2 rows */
    ck_assert_int_eq(count_rows("hello world", 0, 0, 8), 2);
}
END_TEST

START_TEST(test_count_rows_newline)
{
    /* Explicit newline */
    ck_assert_int_eq(count_rows("hello\nworld", 0, 0, 40), 2);
}
END_TEST

START_TEST(test_count_rows_empty)
{
    /* Empty string — at least 1 row */
    ck_assert_int_eq(count_rows("", 0, 0, 40), 1);
}
END_TEST

/* -----------------------------------------------------------------------
 * Suite assembly
 * --------------------------------------------------------------------- */

static Suite *utf8_suite(void)
{
    Suite *s = suite_create("utf8");

    TCase *tc_clen = tcase_create("utf8_clen");
    tcase_add_test(tc_clen, test_clen_ascii);
    tcase_add_test(tc_clen, test_clen_two_byte);
    tcase_add_test(tc_clen, test_clen_three_byte);
    tcase_add_test(tc_clen, test_clen_four_byte);
    tcase_add_test(tc_clen, test_clen_nul);
    suite_add_tcase(s, tc_clen);

    TCase *tc_cwidth = tcase_create("utf8_cwidth");
    tcase_add_test(tc_cwidth, test_cwidth_ascii);
    tcase_add_test(tc_cwidth, test_cwidth_latin_ext);
    tcase_add_test(tc_cwidth, test_cwidth_cjk);
    tcase_add_test(tc_cwidth, test_cwidth_emoji);
    suite_add_tcase(s, tc_cwidth);

    TCase *tc_wrap = tcase_create("wrap_line_end");
    tcase_add_test(tc_wrap, test_wrap_fits_exactly);
    tcase_add_test(tc_wrap, test_wrap_breaks_at_space);
    tcase_add_test(tc_wrap, test_wrap_newline_terminates);
    tcase_add_test(tc_wrap, test_wrap_no_space_hard_break);
    suite_add_tcase(s, tc_wrap);

    TCase *tc_rows = tcase_create("count_rows");
    tcase_add_test(tc_rows, test_count_rows_single);
    tcase_add_test(tc_rows, test_count_rows_wraps);
    tcase_add_test(tc_rows, test_count_rows_newline);
    tcase_add_test(tc_rows, test_count_rows_empty);
    suite_add_tcase(s, tc_rows);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(utf8_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (failed == 0) ? 0 : 1;
}
