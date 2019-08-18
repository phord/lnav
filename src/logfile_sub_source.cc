/**
 * Copyright (c) 2007-2012, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <future>
#include <algorithm>
#include <sqlite3.h>

#include "k_merge_tree.h"
#include "lnav_util.hh"
#include "log_accel.hh"
#include "relative_time.hh"
#include "logfile_sub_source.hh"
#include "command_executor.hh"
#include "ansi_scrubber.hh"
#include "lnav_config.hh"

using namespace std;

bookmark_type_t logfile_sub_source::BM_ERRORS("error");
bookmark_type_t logfile_sub_source::BM_WARNINGS("warning");
bookmark_type_t logfile_sub_source::BM_FILES("");

static int pretty_sql_callback(exec_context &ec, sqlite3_stmt *stmt)
{
    if (!sqlite3_stmt_busy(stmt)) {
        return 0;
    }

    int ncols = sqlite3_column_count(stmt);

    for (int lpc = 0; lpc < ncols; lpc++) {
        if (!ec.ec_accumulator.empty()) {
            ec.ec_accumulator.append(", ");
        }

        const char *res = (const char *)sqlite3_column_text(stmt, lpc);
        if (res == nullptr) {
            continue;
        }

        ec.ec_accumulator.append(res);
    }

    return 0;
}

static future<string> pretty_pipe_callback(exec_context &ec,
                                           const string &cmdline,
                                           auto_fd &fd)
{
    auto retval = std::async(std::launch::async, [&]() {
        char buffer[1024];
        ostringstream ss;
        ssize_t rc;

        while ((rc = read(fd, buffer, sizeof(buffer))) > 0) {
            ss.write(buffer, rc);
        }

        string retval = ss.str();

        if (endswith(retval.c_str(), "\n")) {
            retval.resize(retval.length() - 1);
        }

        return retval;
    });

    return retval;
}

logfile_sub_source::logfile_sub_source()
    : lss_flags(0),
      lss_force_rebuild(false),
      lss_token_file(NULL),
      lss_min_log_level(LEVEL_UNKNOWN),
      lss_marked_only(false),
      lss_index_delegate(NULL),
      lss_longest_line(0),
      lss_meta_grepper(*this),
      lss_location_history(*this)
{
    this->tss_supports_filtering = true;
    this->clear_line_size_cache();
    this->clear_min_max_log_times();
}

logfile_sub_source::~logfile_sub_source()
{ }

shared_ptr<logfile> logfile_sub_source::find(const char *fn,
                                  content_line_t &line_base)
{
    iterator iter;
    shared_ptr<logfile> retval = NULL;

    line_base = content_line_t(0);
    for (iter = this->lss_files.begin();
         iter != this->lss_files.end() && retval == NULL;
         iter++) {
        logfile_data &ld = *(*iter);
        if (ld.get_file() == NULL) {
            continue;
        }
        if (strcmp(ld.get_file()->get_filename().c_str(), fn) == 0) {
            retval = ld.get_file();
        }
        else {
            line_base += content_line_t(MAX_LINES_PER_FILE);
        }
    }

    return retval;
}

vis_line_t logfile_sub_source::find_from_time(const struct timeval &start)
{
    vector<uint32_t>::iterator lb;
    vis_line_t retval(-1);

    lb = lower_bound(this->lss_filtered_index.begin(),
                     this->lss_filtered_index.end(),
                     start,
                     filtered_logline_cmp(*this));
    if (lb != this->lss_filtered_index.end()) {
        retval = vis_line_t(lb - this->lss_filtered_index.begin());
    }

    return retval;
}

void logfile_sub_source::text_value_for_line(textview_curses &tc,
                                             int row,
                                             string &value_out,
                                             line_flags_t flags)
{
    content_line_t line(0);

    require(row >= 0);
    require((size_t)row < this->lss_filtered_index.size());

    line = this->at(vis_line_t(row));

    if (flags & RF_RAW) {
        shared_ptr<logfile> lf = this->find(line);
        value_out = lf->read_line(lf->begin() + line)
            .map([](auto sbr) { return to_string(sbr); })
            .unwrapOr({});
        return;
    }

    this->lss_token_flags = flags;
    this->lss_token_file   = this->find(line);
    this->lss_token_line   = this->lss_token_file->begin() + line;

    this->lss_token_attrs.clear();
    this->lss_token_values.clear();
    this->lss_share_manager.invalidate_refs();
    if (flags & text_sub_source::RF_FULL) {
        shared_buffer_ref sbr;

        this->lss_token_file->read_full_message(this->lss_token_line, sbr);
        this->lss_token_value = to_string(sbr);
    } else {
        this->lss_token_value =
            this->lss_token_file->read_line(this->lss_token_line).map([](auto sbr) {
                return to_string(sbr);
            }).unwrapOr({});
    }
    this->lss_token_shift_start = 0;
    this->lss_token_shift_size = 0;

    log_format *format = this->lss_token_file->get_format();

    value_out = this->lss_token_value;
    if (this->lss_flags & F_SCRUB) {
        format->scrub(value_out);
    }

    shared_buffer_ref sbr;

    sbr.share(this->lss_share_manager,
              (char *)this->lss_token_value.c_str(), this->lss_token_value.size());
    format->annotate(line, sbr, this->lss_token_attrs, this->lss_token_values,
                     false);
    if (this->lss_token_line->get_sub_offset() != 0) {
        this->lss_token_attrs.clear();
    }
    if (flags & RF_REWRITE) {
        exec_context ec(&this->lss_token_values, pretty_sql_callback, pretty_pipe_callback);
        string rewritten_line;

        ec.ec_top_line = vis_line_t(row);
        add_ansi_vars(ec.ec_global_vars);
        add_global_vars(ec);
        format->rewrite(ec, sbr, this->lss_token_attrs, rewritten_line);
        this->lss_token_value.assign(rewritten_line);
        value_out = this->lss_token_value;
    }

    if ((this->lss_token_file->is_time_adjusted() ||
         format->lf_timestamp_flags & ETF_MACHINE_ORIENTED) &&
        format->lf_date_time.dts_fmt_lock != -1) {
        auto time_attr = find_string_attr(
            this->lss_token_attrs, &logline::L_TIMESTAMP);
        if (time_attr != this->lss_token_attrs.end()) {
            const struct line_range time_range = time_attr->sa_range;
            struct timeval adjusted_time;
            struct exttm adjusted_tm;
            char buffer[128];
            const char *fmt;
            ssize_t len;

            if (format->lf_timestamp_flags & ETF_MACHINE_ORIENTED) {
                format->lf_date_time.convert_to_timeval(
                    &this->lss_token_value.c_str()[time_range.lr_start],
                    time_range.length(),
                    format->get_timestamp_formats(),
                    adjusted_time);
                fmt = "%Y-%m-%d %H:%M:%S.%f";
                gmtime_r(&adjusted_time.tv_sec, &adjusted_tm.et_tm);
                adjusted_tm.et_nsec = adjusted_time.tv_usec * 1000;
                len = ftime_fmt(buffer, sizeof(buffer), fmt, adjusted_tm);
            } else {
                adjusted_time = this->lss_token_line->get_timeval();
                gmtime_r(&adjusted_time.tv_sec, &adjusted_tm.et_tm);
                adjusted_tm.et_nsec = adjusted_time.tv_usec * 1000;
                len = format->lf_date_time.ftime(buffer, sizeof(buffer),
                                                 adjusted_tm);
            }

            if (len > time_range.length()) {
                ssize_t padding = len - time_range.length();

                value_out.insert(time_range.lr_start,
                                 padding,
                                 ' ');
            }
            value_out.replace(time_range.lr_start,
                              len,
                              buffer,
                              len);
            this->lss_token_shift_start = time_range.lr_start;
            this->lss_token_shift_size = len - time_range.length();
        }
    }

    if (this->lss_flags & F_FILENAME || this->lss_flags & F_BASENAME) {
        size_t file_offset_end;
        std::string name;
        if (this->lss_flags & F_FILENAME) {
            file_offset_end = this->lss_filename_width;
            name = this->lss_token_file->get_filename();
            if (file_offset_end < name.size()) {
                file_offset_end = name.size();
                this->lss_filename_width = name.size();
            }
        } else {
            file_offset_end = this->lss_basename_width;
            name = this->lss_token_file->get_unique_path();
            if (file_offset_end < name.size()) {
                file_offset_end = name.size();
                this->lss_basename_width = name.size();
            }
        }
        value_out.insert(0, file_offset_end - name.size() + 1, ' ');
        value_out.insert(0, name);
    } else {
        // Insert space for the file/search-hit markers.
        value_out.insert(0, 1, ' ');
    }

    if (this->lss_flags & F_TIME_OFFSET) {
        int64_t curr_millis, diff;

        curr_millis = this->lss_token_line->get_time_in_millis();

        vis_line_t prev_mark =
            tc.get_bookmarks()[&textview_curses::BM_USER].prev(vis_line_t(row));
        vis_line_t next_mark =
            tc.get_bookmarks()[&textview_curses::BM_USER].next(vis_line_t(row));
        if (prev_mark == -1 && next_mark != -1) {
            auto next_line = this->find_line(this->at(next_mark));

            diff = curr_millis - next_line->get_time_in_millis();
        } else {
            if (prev_mark == -1) {
                prev_mark = 0_vl;
            }

            auto first_line = this->find_line(this->at(prev_mark));
            auto start_millis = first_line->get_time_in_millis();
            diff = curr_millis - start_millis;
        }

        value_out = "|" + value_out;
        string relstr;
        size_t rel_length = duration2str(diff, relstr);
        value_out.insert(0, relstr);
        if (rel_length < 12) {
            value_out.insert(0, 12 - rel_length, ' ');
        }
    }
}

void logfile_sub_source::text_attrs_for_line(textview_curses &lv,
                                             int row,
                                             string_attrs_t &value_out)
{
    view_colors &     vc        = view_colors::singleton();
    logline *         next_line = NULL;
    struct line_range lr;
    int time_offset_end = 0;
    int attrs           = 0;

    value_out = this->lss_token_attrs;

    attrs = vc.vc_level_attrs[this->lss_token_line->get_msg_level()].first;

    if ((row + 1) < (int)this->lss_filtered_index.size()) {
        next_line = this->find_line(this->at(vis_line_t(row + 1)));
    }

    if (next_line != NULL &&
        (day_num(next_line->get_time()) >
         day_num(this->lss_token_line->get_time()))) {
        attrs |= A_UNDERLINE;
    }

    const std::vector<logline_value> &line_values = this->lss_token_values;

    lr.lr_start = 0;
    lr.lr_end = this->lss_token_value.length();
    value_out.emplace_back(lr, &textview_curses::SA_ORIGINAL_LINE);

    lr.lr_start = time_offset_end;
    lr.lr_end   = -1;

    value_out.emplace_back(lr, &view_curses::VC_STYLE, attrs);

    for (const auto &line_value : line_values) {
        if ((!(this->lss_token_flags & RF_FULL) &&
            line_value.lv_sub_offset != this->lss_token_line->get_sub_offset()) ||
            !line_value.lv_origin.is_valid()) {
            continue;
        }

        if (line_value.lv_hidden) {
            value_out.emplace_back(
                line_value.lv_origin, &textview_curses::SA_HIDDEN);
        }

        if (!line_value.lv_identifier || !line_value.lv_origin.is_valid()) {
            continue;
        }

        int id_attrs = vc.attrs_for_ident(line_value.text_value(),
                                          line_value.text_length());

        line_range ident_range = line_value.lv_origin;
        if (this->lss_token_flags & RF_FULL) {
            ident_range = line_value.origin_in_full_msg(
                this->lss_token_value.c_str(), this->lss_token_value.length());
        }

        value_out.emplace_back(ident_range, &view_curses::VC_STYLE, id_attrs);
    }

    if (this->lss_token_shift_size) {
        shift_string_attrs(value_out, this->lss_token_shift_start + 1,
                           this->lss_token_shift_size);
    }

    shift_string_attrs(value_out, 0, 1);

    lr.lr_start = 0;
    lr.lr_end = 1;
    {
        vis_bookmarks &bm = lv.get_bookmarks();
        bookmark_vector<vis_line_t> &bv = bm[&BM_FILES];
        bool is_first_for_file = binary_search(
            bv.begin(), bv.end(), vis_line_t(row));
        bool is_last_for_file = binary_search(
            bv.begin(), bv.end(), vis_line_t(row + 1));
        chtype graph = ACS_VLINE;
        if (is_first_for_file) {
            if (is_last_for_file) {
                graph = ACS_HLINE;
            }
            else {
                graph = ACS_ULCORNER;
            }
        }
        else if (is_last_for_file) {
            graph = ACS_LLCORNER;
        }
        value_out.push_back(
            string_attr(lr, &view_curses::VC_GRAPHIC, graph));

        if (!(this->lss_token_flags & RF_FULL)) {
            bookmark_vector<vis_line_t> &bv_search = bm[&textview_curses::BM_SEARCH];

            if (binary_search(::begin(bv_search), ::end(bv_search),
                              vis_line_t(row))) {
                lr.lr_start = 0;
                lr.lr_end = 1;
                value_out.emplace_back(lr, &view_curses::VC_STYLE, A_REVERSE);
            }
        }
    }

    value_out.emplace_back(lr, &view_curses::VC_STYLE, vc.attrs_for_ident(
        this->lss_token_file->get_filename()));

    if (this->lss_flags & F_FILENAME || this->lss_flags & F_BASENAME) {
        size_t file_offset_end = (this->lss_flags & F_FILENAME) ?
                                    this->lss_filename_width :
                                    this->lss_basename_width ;

        shift_string_attrs(value_out, 0, file_offset_end);

        lr.lr_start = 0;
        lr.lr_end   = file_offset_end + 1;
        value_out.emplace_back(lr, &view_curses::VC_STYLE, vc.attrs_for_ident(
            this->lss_token_file->get_filename()));
    }

    if (this->lss_flags & F_TIME_OFFSET) {
        time_offset_end = 13;
        lr.lr_start     = 0;
        lr.lr_end       = time_offset_end;

        shift_string_attrs(value_out, 0, time_offset_end);

        attrs = vc.attrs_for_role(view_colors::VCR_OFFSET_TIME);
        value_out.emplace_back(lr, &view_curses::VC_STYLE, attrs);
        value_out.emplace_back(line_range(12, 13),
            &view_curses::VC_GRAPHIC, ACS_VLINE);

        int bar_attrs = 0;

        switch (this->get_line_accel_direction(vis_line_t(row))) {
        case log_accel::A_STEADY:
            break;
        case log_accel::A_DECEL:
            bar_attrs = vc.attrs_for_role(view_colors::VCR_DIFF_DELETE);
            break;
        case log_accel::A_ACCEL:
            bar_attrs = vc.attrs_for_role(view_colors::VCR_DIFF_ADD);
            break;
        }
        value_out.push_back(
            string_attr(line_range(12, 13), &view_curses::VC_STYLE, bar_attrs));
    }

    lr.lr_start = 0;
    lr.lr_end   = -1;
    value_out.emplace_back(lr, &logline::L_FILE, this->lss_token_file.get());
    value_out.emplace_back(lr, &textview_curses::SA_FORMAT,
                           this->lss_token_file->get_format()->get_name());

    {
        bookmark_vector<vis_line_t> &bv = lv.get_bookmarks()[&textview_curses::BM_META];
        bookmark_vector<vis_line_t>::iterator bv_iter;

        bv_iter = lower_bound(bv.begin(), bv.end(), vis_line_t(row + 1));
        if (bv_iter != bv.begin()) {
            --bv_iter;
            content_line_t part_start_line = this->at(*bv_iter);
            std::map<content_line_t, bookmark_metadata>::iterator bm_iter;

            if ((bm_iter = this->lss_user_mark_metadata.find(part_start_line))
                != this->lss_user_mark_metadata.end() &&
                !bm_iter->second.bm_name.empty()) {
                lr.lr_start = 0;
                lr.lr_end   = -1;
                value_out.emplace_back(lr, &logline::L_PARTITION, &bm_iter->second);
            }
        }

        auto bm_iter = this->lss_user_mark_metadata.find(this->at(vis_line_t(row)));

        if (bm_iter != this->lss_user_mark_metadata.end()) {
            lr.lr_start = 0;
            lr.lr_end = -1;
            value_out.emplace_back(lr, &logline::L_META, &bm_iter->second);
        }
    }

    if (this->lss_token_file->is_time_adjusted()) {
        struct line_range time_range = find_string_attr_range(
            value_out, &logline::L_TIMESTAMP);

        if (time_range.lr_end != -1) {
            attrs = vc.attrs_for_role(view_colors::VCR_ADJUSTED_TIME);
            value_out.emplace_back(time_range, &view_curses::VC_STYLE, attrs);
        }
    }
    else if ((((this->lss_token_line->get_time() / (5 * 60)) % 2) == 0) &&
             !this->lss_token_line->is_continued()) {
        struct line_range time_range = find_string_attr_range(
            value_out, &logline::L_TIMESTAMP);

        if (time_range.lr_end != -1) {
            attrs = vc.attrs_for_role(view_colors::VCR_ALT_ROW);
            value_out.emplace_back(time_range, &view_curses::VC_STYLE, attrs);
        }
    }

    if (this->lss_token_line->is_time_skewed()) {
        struct line_range time_range = find_string_attr_range(
            value_out, &logline::L_TIMESTAMP);

        if (time_range.lr_end != -1) {
            attrs = vc.attrs_for_role(view_colors::VCR_SKEWED_TIME);
            value_out.emplace_back(time_range, &view_curses::VC_STYLE, attrs);
        }
    }
}

logfile_sub_source::rebuild_result logfile_sub_source::rebuild_index()
{
    iterator iter;
    size_t total_lines = 0;
    bool full_sort = false;
    int file_count = 0;
    bool force = this->lss_force_rebuild;
    // FIXME: Handle external force requests more gently
    rebuild_result retval = rebuild_result::rr_no_change;

    this->lss_force_rebuild = false;
    if (force) {
        retval = rebuild_result::rr_full_rebuild;
    }

    // Find new lines in all files
    for (iter = this->lss_files.begin();
         iter != this->lss_files.end();
         iter++) {
        logfile_data &ld = *(*iter);

        if (ld.get_file() == NULL) {
            if (ld.ld_lines_indexed > 0) {
                force  = true;
                retval = rebuild_result::rr_full_rebuild;
            }
        }
        else {
            logfile &lf = *ld.get_file();

            if (!this->tss_view->is_paused()) {

                // Find new lines in this file; results are in lf
                // FIXME: Change this so we _never_ rebuild the index.  So the return value here
                // doesn't matter.  But maybe we have some lines that need to be inserted into the
                // existing index.  We can find those with the time_skew flag, but we also need to
                // find ones that are before our most recent line(s). So, let's pass the latest time
                // in to rebuild_index so he can tell us which of his lines are already out of order
                // wrt that and to each other.
                auto rebuild = lf.rebuild_index(/* last_indexed_line->get_timeval() */);
                if (rebuild == logfile::RR_NO_NEW_LINES) {
                        // No new lines, but do we still have lines we haven't processed?
                        if (ld.ld_lines_indexed < lf.size()) {
                            rebuild = logfile::RR_NEW_LINES;
                        }
                }
                switch (rebuild) {
                    case logfile::RR_NO_NEW_LINES:
                        // No changes
                        break;
                    case logfile::RR_NEW_LINES:
                        if (retval == rebuild_result::rr_no_change) {
                            retval = rebuild_result::rr_appended_lines;
                        }
                        if (!this->lss_index.empty()) {
                            logline &new_file_line = lf[ld.ld_lines_indexed];
                            content_line_t cl = this->lss_index.back();
                            logline *last_indexed_line = this->find_line(cl);

                            // If there are new lines that are older than what we
                            // have in the index, we need to resort.
                            if (last_indexed_line == nullptr ||

                            // FIXME: Change this to find the position in the existing sort where these new lines
                            // would be inserted. Then remove lines to that position from all files, and run
                            // merge-insert again.  Add a new rebuild_result, rr_inserted_lines.
                                new_file_line <
                                last_indexed_line->get_timeval()) {
                                force = true;
                                retval = rebuild_result::rr_full_rebuild;
                            }
                        }
                        break;
                    case logfile::RR_INVALID:
                    case logfile::RR_NEW_ORDER:
                        retval = rebuild_result::rr_full_rebuild;
                        force = true;
                        break;
                }
            }
            file_count += 1;
            total_lines += lf.size();
        }
    }

    // big_array::reserve is non-destructive
    if (this->lss_index.reserve(total_lines)) {
        force = true;
    }

    constexpr bool never_force = true;
    if (force && !never_force) {
        full_sort = true;

        // FORCE: reset indexed lines in all files back to zero
        for (iter = this->lss_files.begin();
             iter != this->lss_files.end();
             iter++) {
            (*iter)->ld_lines_indexed = 0;
        }

        // FORCE: wipe out indexed data
        this->lss_index.clear();
        this->lss_filtered_index.clear();
        this->lss_longest_line = 0;
        this->lss_basename_width = 0;
        this->lss_filename_width = 0;
    }

    if (retval != rebuild_result::rr_no_change || force) {
        size_t start_size = this->lss_index.size();

        // Get longest lines, widest filenames, etc, among all files
        for (auto ld : this->lss_files) {
            std::shared_ptr<logfile> lf = ld->get_file();

            if (lf == nullptr) {
                continue;
            }
            this->lss_longest_line = std::max(
                this->lss_longest_line, lf->get_longest_line_length());
            this->lss_basename_width = std::max(
                this->lss_basename_width, lf->get_unique_path().size());
            this->lss_filename_width = std::max(
                this->lss_filename_width, lf->get_filename().size());
        }

        // Sort the whole thing
        if (full_sort) {
            for (auto ld : this->lss_files) {
                shared_ptr<logfile> lf = ld->get_file();

                if (lf == nullptr) {
                    continue;
                }

                // Recreate lss_index from scratch as (HIGH:file_index :: LOW:line_number)
                for (size_t line_index = 0; line_index < lf->size(); line_index++) {
                    content_line_t con_line(ld->ld_file_index * MAX_LINES_PER_FILE +
                                            line_index);

                    this->lss_index.push_back(con_line);
                }
            }

            log_info("DELAY> Sorting");

            // XXX get rid of this full sort on the initial run, it's not
            // needed unless the file is not in time-order
            logline_cmp line_cmper(*this);
            sort(this->lss_index.begin(), this->lss_index.end(), line_cmper);
            log_info("DELAY< Sorting");
        } else {
            kmerge_tree_c<logline, logfile_data, logfile::iterator> merge(
                file_count);

            for (iter = this->lss_files.begin();
                 iter != this->lss_files.end();
                 iter++) {
                logfile_data *ld = *iter;
                shared_ptr<logfile> lf = ld->get_file();
                if (lf == NULL) {
                    continue;
                }

                auto begin = lf->begin() + ld->ld_lines_indexed;
                if (begin == lf->end()) {
                    continue;
                }

                merge.add(ld, begin, lf->end());
            }

            merge.execute();
            for (;;) {
                logfile::iterator lf_iter;
                logfile_data *ld;

                if (!merge.get_top(ld, lf_iter)) {
                    break;
                }


                int file_index = ld->ld_file_index;
                int line_index = ld->ld_lines_indexed++;
                ensure(lf_iter - ld->get_file()->begin() == line_index);

                // Add onto lss_index as (HIGH:file_index :: LOW:line_number), already sorted
                content_line_t con_line(file_index * MAX_LINES_PER_FILE +
                                        line_index);

                this->lss_index.push_back(con_line);

                if (lf_iter+1 == ld->get_file()->end())
                {
                    // stop when we consume the last line of any source file being merged
                    break;
                }
                merge.next();
            }
        }

        if (this->lss_filtered_index.empty()) {
            this->lss_filtered_index.reserve(this->lss_index.size());
        }

        uint32_t filter_in_mask, filter_out_mask;
        this->get_filters().get_enabled_mask(filter_in_mask, filter_out_mask);

        // notify the delegate (might need work here; histograms?)
        if (start_size == 0 && this->lss_index_delegate != NULL) {
            this->lss_index_delegate->index_start(*this);
        }

        // Build lss_filtered_index: vector of lss_index indexes
        // FIXME: If lss_index changes because of order sorting, related
        //  filtered_index changes, too.  It should be easy to apply the
        //  changes, though, since the lss_index items should always move
        //  forward by some regular amount.
        for (size_t index_index = start_size;
             index_index < this->lss_index.size();
             index_index++) {
            content_line_t cl = (content_line_t) this->lss_index[index_index];
            uint64_t line_number;
            logfile_data *ld = this->find_data(cl, line_number);
            auto line_iter = ld->get_file()->begin() + line_number;

            if (!ld->ld_filter_state.excluded(filter_in_mask, filter_out_mask,
                    line_number) && this->check_extra_filters(*line_iter)) {
                this->lss_filtered_index.push_back(index_index);
                if (this->lss_index_delegate != NULL) {
                    shared_ptr<logfile> lf = ld->get_file();
                    this->lss_index_delegate->index_line(
                            *this, lf.get(), lf->begin() + line_number);
                }
            }
        }

        // notify the delegate (might need work here; histograms?)
        if (this->lss_index_delegate != nullptr) {
            this->lss_index_delegate->index_complete(*this);
        }
    }

    // FIXME: (Somewhere else...)  after lss_filtered_index is rebuilt, the existing
    // search (highlight, bookmarks, etc.) is reapplied from scratch. Instead it should be simple
    // enough to modify the existing search results.
    //   If the new filter removed lines, just remove those same lines from the search results.
    //   If the new filter added lines, check only those added lines to update the search results.
    //
    // Maybe what is needed is some notification call like this:
    //    filter_changed(added_lines_vec, removed_lines_vec);

    if (retval == rebuild_result::rr_full_rebuild && never_force) {
        retval = rebuild_result::rr_appended_lines;
    }

    switch (retval) {
        case rebuild_result::rr_no_change:
            break;
        case rebuild_result::rr_full_rebuild:
            this->tss_view->redo_search();
            break;
        case rebuild_result::rr_appended_lines:
            this->tss_view->search_new_data();
            break;
    }

    return retval;
}

void logfile_sub_source::text_update_marks(vis_bookmarks &bm)
{
    shared_ptr<logfile> last_file = nullptr;
    vis_line_t vl;

    bm[&BM_WARNINGS].clear();
    bm[&BM_ERRORS].clear();
    bm[&BM_FILES].clear();

    for (auto &lss_user_mark : this->lss_user_marks) {
        bm[lss_user_mark.first].clear();
    }

    for (; vl < (int)this->lss_filtered_index.size(); ++vl) {
        const content_line_t orig_cl = this->at(vl);
        content_line_t cl = orig_cl;
        shared_ptr<logfile> lf;

        lf = this->find(cl);

        for (auto &lss_user_mark : this->lss_user_marks) {
            if (binary_search(lss_user_mark.second.begin(),
                              lss_user_mark.second.end(),
                              orig_cl)) {
                bm[lss_user_mark.first].insert_once(vl);

                if (lss_user_mark.first == &textview_curses::BM_USER) {
                    auto ll = lf->begin() + cl;

                    ll->set_mark(true);
                }
            }
        }

        if (lf != last_file) {
            bm[&BM_FILES].insert_once(vl);
        }

        auto line_iter = lf->begin() + cl;
        if (!line_iter->is_continued()) {
            switch (line_iter->get_msg_level()) {
                case LEVEL_WARNING:
                    bm[&BM_WARNINGS].insert_once(vl);
                    break;

                case LEVEL_FATAL:
                case LEVEL_ERROR:
                case LEVEL_CRITICAL:
                    bm[&BM_ERRORS].insert_once(vl);
                    break;

                default:
                    break;
            }
        }

        last_file = lf;
    }
}

log_accel::direction_t logfile_sub_source::get_line_accel_direction(
    vis_line_t vl)
{
    log_accel la;

    while (vl >= 0) {
        logline *curr_line = this->find_line(this->at(vl));

        if (curr_line->is_continued()) {
            --vl;
            continue;
        }

        if (!la.add_point(curr_line->get_time_in_millis())) {
            break;
        }

        --vl;
    }

    return la.get_direction();
}

void logfile_sub_source::text_filters_changed()
{
    for (auto ld : *this) {
        shared_ptr<logfile> lf = ld->get_file();

        if (lf != nullptr) {
            ld->ld_filter_state.clear_deleted_filter_state();
            lf->reobserve_from(lf->begin() + ld->ld_filter_state.get_min_count(lf->size()));
        }
    }

    uint32_t filtered_in_mask, filtered_out_mask;

    this->get_filters().get_enabled_mask(filtered_in_mask, filtered_out_mask);

    if (this->lss_index_delegate != nullptr) {
        this->lss_index_delegate->index_start(*this);
    }

    this->lss_filtered_index.clear();
    for (size_t index_index = 0; index_index < this->lss_index.size(); index_index++) {
        content_line_t cl = (content_line_t) this->lss_index[index_index];
        uint64_t line_number;
        logfile_data *ld = this->find_data(cl, line_number);
        auto line_iter = ld->get_file()->begin() + line_number;

        if (!ld->ld_filter_state.excluded(filtered_in_mask, filtered_out_mask,
                line_number) && this->check_extra_filters(*line_iter)) {
            this->lss_filtered_index.push_back(index_index);
            if (this->lss_index_delegate != nullptr) {
                shared_ptr<logfile> lf = ld->get_file();
                this->lss_index_delegate->index_line(
                        *this, lf.get(), lf->begin() + line_number);
            }
        }
    }

    if (this->lss_index_delegate != nullptr) {
        this->lss_index_delegate->index_complete(*this);
    }

    if (this->tss_view != nullptr) {
        this->tss_view->reload_data();
        this->tss_view->redo_search();
    }
}

bool logfile_sub_source::list_input_handle_key(listview_curses &lv, int ch)
{
    switch (ch) {
        case 'h':
        case 'H':
        case KEY_SLEFT:
        case KEY_LEFT:
            if (lv.get_left() == 0) {
                this->increase_line_context();
                lv.set_needs_update();
                return true;
            }
            break;
        case 'l':
        case 'L':
        case KEY_SRIGHT:
        case KEY_RIGHT:
            if (this->decrease_line_context()) {
                lv.set_needs_update();
                return true;
            }
            break;
    }
    return false;
}

nonstd::optional<pair<grep_proc_source<vis_line_t> *, grep_proc_sink<vis_line_t> *>>
logfile_sub_source::get_grepper()
{
    return make_pair(
        (grep_proc_source<vis_line_t> *) &this->lss_meta_grepper,
        (grep_proc_sink<vis_line_t> *) &this->lss_meta_grepper
    );
}

void log_location_history::loc_history_append(vis_line_t top)
{
    if (top >= this->llh_log_source.text_line_count()) {
        return;
    }

    content_line_t cl = this->llh_log_source.at(top);

    auto iter = this->llh_history.begin();
    iter += this->llh_history.size() - this->lh_history_position;
    this->llh_history.erase_from(iter);
    this->lh_history_position = 0;
    this->llh_history.push_back(cl);
}

nonstd::optional<vis_line_t> log_location_history::loc_history_back(vis_line_t current_top)
{
    while (this->lh_history_position < this->llh_history.size()) {
        auto iter = this->llh_history.rbegin();

        auto vis_for_pos = this->llh_log_source.find_from_content(*iter);

        if (this->lh_history_position == 0 && vis_for_pos != current_top) {
            return vis_for_pos;
        }

        if ((this->lh_history_position + 1) >= this->llh_history.size()) {
            break;
        }

        this->lh_history_position += 1;

        iter += this->lh_history_position;

        vis_for_pos = this->llh_log_source.find_from_content(*iter);

        if (vis_for_pos) {
            return vis_for_pos;
        }
    }

    return nonstd::nullopt;
}

nonstd::optional<vis_line_t>
log_location_history::loc_history_forward(vis_line_t current_top)
{
    while (this->lh_history_position > 0) {
        this->lh_history_position -= 1;

        auto iter = this->llh_history.rbegin();

        iter += this->lh_history_position;

        auto vis_for_pos = this->llh_log_source.find_from_content(*iter);

        if (vis_for_pos) {
            return vis_for_pos;
        }
    }

    return nonstd::nullopt;
}
