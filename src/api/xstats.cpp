// #5281, #5284 - #5287, #5291, #5317, #5329, #5342, #5343, #5346: a stats view is a Steam leaderboard.
#include "xlive/xfuncs.h"
#include "api/xlive.h"
#include "api/xsession.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/overlapped.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace {

std::recursive_mutex g_mutex;

// --- Leaderboard handles -------------------------------------------------------------------------

struct LeaderboardLookup {
	uint32_t viewId = 0;
	SteamLeaderboard_t handle = 0;
	bool failed = false;
	std::shared_ptr<xls::SteamCall<LeaderboardFindResult_t>> call;

	// True once the handle is known, found or not.
	bool Poll()
	{
		if (handle || failed) {
			return true;
		}
		if (!call) {
			if (!xls::SteamReady()) {
				failed = true;
				return true;
			}
			std::string name = xls::LeaderboardName(viewId);
			const xls::LeaderboardMapping* mapping = xls::LeaderboardFor(viewId);
			bool create = mapping ? mapping->createIfMissing : xls::Cfg().leaderboardsCreateIfMissing;
			call = std::make_shared<xls::SteamCall<LeaderboardFindResult_t>>();
			if (create) {
				ELeaderboardSortMethod sort = (mapping && mapping->ascending) ? k_ELeaderboardSortMethodAscending : k_ELeaderboardSortMethodDescending;
				ELeaderboardDisplayType display = (ELeaderboardDisplayType)(mapping ? mapping->displayType : 1);
				call->Start(xls::SteamUserStats()->FindOrCreateLeaderboard(name.c_str(), sort, display));
			}
			else {
				call->Start(xls::SteamUserStats()->FindLeaderboard(name.c_str()));
			}
		}
		if (!call->Poll()) {
			return false;
		}
		if (call->failed || !call->result.m_bLeaderboardFound) {
			XLS_LOG_ERROR("stats: leaderboard %s for view %u not found on Steam.", xls::LeaderboardName(viewId).c_str(), viewId);
			failed = true;
		}
		else {
			handle = call->result.m_hSteamLeaderboard;
			XLS_LOG_INFO("stats: view %u -> leaderboard %s.", viewId, xls::LeaderboardName(viewId).c_str());
		}
		return true;
	}
};

std::map<uint32_t, std::shared_ptr<LeaderboardLookup>> g_leaderboards;

std::shared_ptr<LeaderboardLookup> Leaderboard(uint32_t viewId)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_leaderboards.find(viewId);
	if (it != g_leaderboards.end()) {
		return it->second;
	}
	auto lookup = std::make_shared<LeaderboardLookup>();
	lookup->viewId = viewId;
	g_leaderboards[viewId] = lookup;
	return lookup;
}

// --- Column layout -------------------------------------------------------------------------------

struct ColumnLayout {
	uint16_t columnId;
	uint8_t type;
	bool isRating;
};

// The columns a view carries, rating first, then the details in SPA order.
std::vector<ColumnLayout> LayoutFor(uint32_t viewId, const std::vector<uint16_t>& requested)
{
	std::vector<ColumnLayout> layout;
	const xls::LeaderboardMapping* mapping = xls::LeaderboardFor(viewId);
	const xls::spa::StatView* view = xls::spa::FindStatView(viewId);
	std::vector<std::pair<uint16_t, uint8_t>> columns;
	if (view) {
		for (const xls::spa::StatColumn& column : view->columns) {
			if (column.isSystem) {
				continue;
			}
			columns.emplace_back(column.columnId, column.type);
		}
	}
	for (uint16_t id : requested) {
		bool known = false;
		for (const auto& column : columns) {
			if (column.first == id) {
				known = true;
				break;
			}
		}
		if (!known) {
			columns.emplace_back(id, (uint8_t)XUSER_DATA_TYPE_INT64);
		}
	}
	if (columns.empty()) {
		return layout;
	}
	int32_t ratingColumn = mapping ? mapping->ratingColumn : -1;
	if (ratingColumn < 0) {
		ratingColumn = columns.front().first;
	}
	for (const auto& column : columns) {
		if (column.first == ratingColumn) {
			layout.push_back(ColumnLayout{ column.first, column.second, true });
			break;
		}
	}
	if (layout.empty()) {
		layout.push_back(ColumnLayout{ (uint16_t)ratingColumn, XUSER_DATA_TYPE_INT64, true });
	}
	if (mapping && !mapping->detailColumns.empty()) {
		for (uint16_t id : mapping->detailColumns) {
			uint8_t type = XUSER_DATA_TYPE_INT32;
			for (const auto& column : columns) {
				if (column.first == id) {
					type = column.second;
				}
			}
			layout.push_back(ColumnLayout{ id, type, false });
		}
	}
	else {
		for (const auto& column : columns) {
			if (column.first != ratingColumn) {
				layout.push_back(ColumnLayout{ column.first, column.second, false });
			}
		}
	}
	return layout;
}

int DetailSlots(uint8_t type)
{
	switch (type) {
		case XUSER_DATA_TYPE_INT64:
		case XUSER_DATA_TYPE_DOUBLE:
		case XUSER_DATA_TYPE_DATETIME:
			return 2;
		case XUSER_DATA_TYPE_UNICODE:
		case XUSER_DATA_TYPE_BINARY:
			return 0;
		default:
			return 1;
	}
}

int32_t ScoreOf(const XUSER_DATA& value)
{
	switch (value.type) {
		case XUSER_DATA_TYPE_INT32:
		case XUSER_DATA_TYPE_CONTEXT:
			return value.nData;
		case XUSER_DATA_TYPE_INT64:
			return value.i64Data > INT32_MAX ? INT32_MAX : (value.i64Data < INT32_MIN ? INT32_MIN : (int32_t)value.i64Data);
		case XUSER_DATA_TYPE_DOUBLE:
			return (int32_t)value.dblData;
		case XUSER_DATA_TYPE_FLOAT:
			return (int32_t)value.fData;
		default:
			return 0;
	}
}

void EncodeDetail(const XUSER_DATA& value, uint8_t type, std::vector<int32_t>& details)
{
	switch (type) {
		case XUSER_DATA_TYPE_INT64:
		case XUSER_DATA_TYPE_DATETIME: {
			int64_t v = value.type == XUSER_DATA_TYPE_INT64 ? value.i64Data : (value.type == XUSER_DATA_TYPE_DATETIME ? (int64_t)(((uint64_t)value.ftData.dwHighDateTime << 32) | value.ftData.dwLowDateTime) : (int64_t)ScoreOf(value));
			details.push_back((int32_t)(v & 0xFFFFFFFF));
			details.push_back((int32_t)(v >> 32));
			break;
		}
		case XUSER_DATA_TYPE_DOUBLE: {
			double d = value.type == XUSER_DATA_TYPE_DOUBLE ? value.dblData : (double)ScoreOf(value);
			int64_t bits;
			memcpy(&bits, &d, sizeof(bits));
			details.push_back((int32_t)(bits & 0xFFFFFFFF));
			details.push_back((int32_t)(bits >> 32));
			break;
		}
		case XUSER_DATA_TYPE_FLOAT: {
			float f = value.type == XUSER_DATA_TYPE_FLOAT ? value.fData : (float)ScoreOf(value);
			int32_t bits;
			memcpy(&bits, &f, sizeof(bits));
			details.push_back(bits);
			break;
		}
		case XUSER_DATA_TYPE_UNICODE:
		case XUSER_DATA_TYPE_BINARY:
			break;
		default:
			details.push_back(ScoreOf(value));
			break;
	}
}

void DecodeColumn(XUSER_DATA& out, uint8_t type, const int32_t* details, int count, int& cursor)
{
	memset(&out, 0, sizeof(out));
	out.type = type;
	int slots = DetailSlots(type);
	if (!slots || cursor + slots > count) {
		if (type == XUSER_DATA_TYPE_UNICODE || type == XUSER_DATA_TYPE_BINARY) {
			out.type = XUSER_DATA_TYPE_NULL;
		}
		cursor += slots;
		return;
	}
	switch (type) {
		case XUSER_DATA_TYPE_INT64:
			out.i64Data = (int64_t)((uint32_t)details[cursor]) | ((int64_t)details[cursor + 1] << 32);
			break;
		case XUSER_DATA_TYPE_DATETIME: {
			uint64_t v = (uint64_t)(uint32_t)details[cursor] | ((uint64_t)(uint32_t)details[cursor + 1] << 32);
			out.ftData.dwLowDateTime = (DWORD)v;
			out.ftData.dwHighDateTime = (DWORD)(v >> 32);
			break;
		}
		case XUSER_DATA_TYPE_DOUBLE: {
			int64_t bits = (int64_t)((uint32_t)details[cursor]) | ((int64_t)details[cursor + 1] << 32);
			memcpy(&out.dblData, &bits, sizeof(bits));
			break;
		}
		case XUSER_DATA_TYPE_FLOAT:
			memcpy(&out.fData, &details[cursor], sizeof(float));
			break;
		default:
			out.nData = details[cursor];
			break;
	}
	cursor += slots;
}

void FillRatingColumn(XUSER_DATA& out, uint8_t type, int32_t score)
{
	memset(&out, 0, sizeof(out));
	out.type = type;
	switch (type) {
		case XUSER_DATA_TYPE_INT64: out.i64Data = score; break;
		case XUSER_DATA_TYPE_DOUBLE: out.dblData = score; break;
		case XUSER_DATA_TYPE_FLOAT: out.fData = (float)score; break;
		case XUSER_DATA_TYPE_UNICODE:
		case XUSER_DATA_TYPE_BINARY: out.type = XUSER_DATA_TYPE_NULL; break;
		default: out.type = XUSER_DATA_TYPE_INT32; out.nData = score; break;
	}
}

// --- Background uploads --------------------------------------------------------------------------

struct Upload {
	uint32_t viewId;
	int32_t score;
	std::vector<int32_t> details;
	std::shared_ptr<LeaderboardLookup> lookup;
	std::shared_ptr<xls::SteamCall<LeaderboardScoreUploaded_t>> call;
	XOVERLAPPED overlapped = {};
};

void StartUpload(std::shared_ptr<Upload> upload)
{
	upload->lookup = Leaderboard(upload->viewId);
	xls::RunAsync(&upload->overlapped, [upload](XOVERLAPPED* overlapped) {
		if (!upload->lookup->Poll()) {
			return false;
		}
		if (upload->lookup->failed) {
			xls::OverlappedComplete(overlapped, ERROR_NOT_FOUND);
			return true;
		}
		if (!upload->call) {
			const xls::LeaderboardMapping* mapping = xls::LeaderboardFor(upload->viewId);
			ELeaderboardUploadScoreMethod method = (mapping && !mapping->keepBest) ? k_ELeaderboardUploadScoreMethodForceUpdate : k_ELeaderboardUploadScoreMethodKeepBest;
			int count = (int)std::min<size_t>(upload->details.size(), k_cLeaderboardDetailsMax);
			upload->call = std::make_shared<xls::SteamCall<LeaderboardScoreUploaded_t>>();
			upload->call->Start(xls::SteamUserStats()->UploadLeaderboardScore(upload->lookup->handle, method, upload->score, count ? upload->details.data() : nullptr, count));
		}
		if (!upload->call->Poll()) {
			return false;
		}
		if (upload->call->failed || !upload->call->result.m_bSuccess) {
			XLS_LOG_WARN("stats: upload to view %u failed.", upload->viewId);
		}
		else {
			XLS_LOG_INFO("stats: view %u score %d uploaded (rank %d).", upload->viewId, upload->score, upload->call->result.m_nGlobalRankNew);
		}
		xls::OverlappedComplete(overlapped, ERROR_SUCCESS);
		return true;
	});
}

// --- Reads ---------------------------------------------------------------------------------------

struct ReadRow {
	XUID xuid;
	DWORD rank;
	int32_t score;
	std::vector<int32_t> details;
	bool present;
};

struct ReadView {
	XUSER_STATS_SPEC spec;
	std::vector<ColumnLayout> layout;
	std::shared_ptr<LeaderboardLookup> lookup;
	std::shared_ptr<xls::SteamCall<LeaderboardScoresDownloaded_t>> call;
	std::vector<ReadRow> rows;
	DWORD totalRows = 0;
	bool done = false;
};

enum class ReadMode { Users, GlobalRange, AroundUser };

struct ReadRequest {
	ReadMode mode = ReadMode::Users;
	std::vector<CSteamID> users;     // Users mode.
	int rangeStart = 0;              // GlobalRange / AroundUser.
	int rangeEnd = 0;
	CSteamID pivot;
	std::vector<ReadView> views;

	// Advances the download of every view. True when all are finished.
	bool Poll()
	{
		bool all = true;
		for (ReadView& view : views) {
			if (view.done) {
				continue;
			}
			all = false;
			if (!view.lookup->Poll()) {
				continue;
			}
			if (view.lookup->failed) {
				FinishEmpty(view);
				continue;
			}
			if (!view.call) {
				view.call = std::make_shared<xls::SteamCall<LeaderboardScoresDownloaded_t>>();
				if (mode == ReadMode::Users) {
					std::vector<CSteamID> ids = users;
					view.call->Start(xls::SteamUserStats()->DownloadLeaderboardEntriesForUsers(view.lookup->handle, ids.data(), (int)ids.size()));
				}
				else if (mode == ReadMode::AroundUser) {
					view.call->Start(xls::SteamUserStats()->DownloadLeaderboardEntries(view.lookup->handle, k_ELeaderboardDataRequestGlobalAroundUser, rangeStart, rangeEnd));
				}
				else {
					view.call->Start(xls::SteamUserStats()->DownloadLeaderboardEntries(view.lookup->handle, k_ELeaderboardDataRequestGlobal, rangeStart, rangeEnd));
				}
			}
			if (!view.call->Poll()) {
				continue;
			}
			view.totalRows = (DWORD)xls::SteamUserStats()->GetLeaderboardEntryCount(view.lookup->handle);
			std::vector<ReadRow> downloaded;
			if (!view.call->failed) {
				for (int i = 0; i < view.call->result.m_cEntryCount; i++) {
					LeaderboardEntry_t entry;
					int32 details[k_cLeaderboardDetailsMax];
					if (!xls::SteamUserStats()->GetDownloadedLeaderboardEntry(view.call->result.m_hSteamLeaderboardEntries, i, &entry, details, k_cLeaderboardDetailsMax)) {
						continue;
					}
					ReadRow row;
					row.xuid = xls::XuidFromSteamId(entry.m_steamIDUser);
					row.rank = (DWORD)entry.m_nGlobalRank;
					row.score = entry.m_nScore;
					row.details.assign(details, details + entry.m_cDetails);
					row.present = true;
					downloaded.push_back(row);
				}
			}
			if (mode == ReadMode::Users) {
				// One row per requested user, in request order, empty when the user has no entry.
				for (CSteamID user : users) {
					XUID xuid = xls::XuidFromSteamId(user);
					bool found = false;
					for (const ReadRow& row : downloaded) {
						if (row.xuid == xuid) {
							view.rows.push_back(row);
							found = true;
							break;
						}
					}
					if (!found) {
						ReadRow empty = {};
						empty.xuid = xuid;
						empty.present = false;
						view.rows.push_back(empty);
					}
				}
			}
			else {
				view.rows = downloaded;
			}
			view.done = true;
		}
		return all;
	}

	void FinishEmpty(ReadView& view)
	{
		if (mode == ReadMode::Users) {
			for (CSteamID user : users) {
				ReadRow empty = {};
				empty.xuid = xls::XuidFromSteamId(user);
				view.rows.push_back(empty);
			}
		}
		view.done = true;
	}

	DWORD RequiredSize(DWORD rowsPerView) const
	{
		DWORD size = sizeof(XUSER_STATS_READ_RESULTS) + (DWORD)views.size() * sizeof(XUSER_STATS_VIEW);
		for (const ReadView& view : views) {
			size += rowsPerView * (sizeof(XUSER_STATS_ROW) + (DWORD)view.layout.size() * sizeof(XUSER_STATS_COLUMN));
		}
		return size;
	}

	// Writes XUSER_STATS_READ_RESULTS into the buffer. Returns false when it does not fit.
	bool Pack(void* buffer, DWORD size) const
	{
		xls::BufferPacker packer(buffer, size);
		XUSER_STATS_READ_RESULTS* results = (XUSER_STATS_READ_RESULTS*)packer.Front(sizeof(XUSER_STATS_READ_RESULTS));
		if (!results) {
			return false;
		}
		results->dwNumViews = (DWORD)views.size();
		results->pViews = (XUSER_STATS_VIEW*)packer.Front(views.size() * sizeof(XUSER_STATS_VIEW));
		if (!results->pViews && !views.empty()) {
			return false;
		}
		for (size_t v = 0; v < views.size(); v++) {
			const ReadView& view = views[v];
			XUSER_STATS_VIEW& outView = results->pViews[v];
			outView.dwViewId = view.spec.dwViewId;
			outView.dwTotalViewRows = view.totalRows;
			outView.dwNumRows = (DWORD)view.rows.size();
			outView.pRows = (XUSER_STATS_ROW*)packer.Front(view.rows.size() * sizeof(XUSER_STATS_ROW));
			if (!outView.pRows && !view.rows.empty()) {
				return false;
			}
			for (size_t r = 0; r < view.rows.size(); r++) {
				const ReadRow& row = view.rows[r];
				XUSER_STATS_ROW& outRow = outView.pRows[r];
				memset(&outRow, 0, sizeof(outRow));
				outRow.xuid = row.xuid;
				outRow.dwRank = row.rank;
				outRow.i64Rating = row.score;
				xls::CopyStringA(outRow.szGamertag, sizeof(outRow.szGamertag), xls::GamertagForXuid(row.xuid).c_str());
				// Columns the title asked for, in its order.
				outRow.dwNumColumns = view.spec.dwNumColumnIds;
				outRow.pColumns = (XUSER_STATS_COLUMN*)packer.Front(outRow.dwNumColumns * sizeof(XUSER_STATS_COLUMN));
				if (!outRow.pColumns && outRow.dwNumColumns) {
					return false;
				}
				for (DWORD c = 0; c < outRow.dwNumColumns; c++) {
					XUSER_STATS_COLUMN& outColumn = outRow.pColumns[c];
					outColumn.wColumnId = view.spec.rgwColumnIds[c];
					memset(&outColumn.Value, 0, sizeof(outColumn.Value));
					outColumn.Value.type = XUSER_DATA_TYPE_NULL;
					int cursor = 0;
					for (const ColumnLayout& column : view.layout) {
						if (column.isRating) {
							if (column.columnId == outColumn.wColumnId) {
								FillRatingColumn(outColumn.Value, column.type, row.score);
							}
							continue;
						}
						if (column.columnId == outColumn.wColumnId) {
							if (row.present) {
								DecodeColumn(outColumn.Value, column.type, row.details.data(), (int)row.details.size(), cursor);
							}
							break;
						}
						cursor += DetailSlots(column.type);
					}
					if (!row.present && outColumn.Value.type == XUSER_DATA_TYPE_NULL) {
						// A user with no entry reads as zeros of the column type.
						uint8_t type = XUSER_DATA_TYPE_INT64;
						for (const ColumnLayout& column : view.layout) {
							if (column.columnId == outColumn.wColumnId) {
								type = column.type;
							}
						}
						FillRatingColumn(outColumn.Value, type, 0);
					}
				}
			}
		}
		return true;
	}
};

std::shared_ptr<ReadRequest> MakeRead(DWORD specCount, const XUSER_STATS_SPEC* specs)
{
	auto request = std::make_shared<ReadRequest>();
	for (DWORD i = 0; i < specCount; i++) {
		ReadView view;
		view.spec = specs[i];
		std::vector<uint16_t> requested(specs[i].rgwColumnIds, specs[i].rgwColumnIds + std::min<DWORD>(specs[i].dwNumColumnIds, XUSER_STATS_ATTRS_IN_SPEC));
		view.spec.dwNumColumnIds = (DWORD)requested.size();
		view.layout = LayoutFor(specs[i].dwViewId, requested);
		view.lookup = Leaderboard(specs[i].dwViewId);
		request->views.push_back(view);
	}
	return request;
}

class StatsEnumerator : public xls::Enumerator {
public:
	std::shared_ptr<ReadRequest> request;
	bool returned = false;

	bool Ready() override
	{
		return !xls::SteamReady() || request->Poll();
	}

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (returned) {
			return ERROR_NO_MORE_FILES;
		}
		if (xls::SteamReady() && !request->Poll()) {
			return ERROR_IO_INCOMPLETE;
		}
		if (!request->Pack(buffer, bufferSize)) {
			return ERROR_INSUFFICIENT_BUFFER;
		}
		returned = true;
		DWORD rows = 0;
		for (const ReadView& view : request->views) {
			rows += (DWORD)view.rows.size();
		}
		*itemsReturned = rows ? rows : 1;
		return ERROR_SUCCESS;
	}
};

DWORD CreateStatsEnumerator(std::shared_ptr<ReadRequest> request, DWORD rows, DWORD* bufferSize, HANDLE* handle)
{
	auto enumerator = std::make_unique<StatsEnumerator>();
	*bufferSize = request->RequiredSize(rows);
	enumerator->requiredBufferSize = *bufferSize;
	enumerator->request = request;
	*handle = xls::EnumeratorRegister(std::move(enumerator));
	return *handle ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

}

// #5317
DWORD WINAPI XSessionWriteStats(HANDLE hSession, XUID xuid, DWORD dwNumViews, const XSESSION_VIEW_PROPERTIES* pViews, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwNumViews || dwNumViews > X_STATS_MAX_VIEWS || !pViews) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::SessionHandleValid(hSession)) {
		XLS_LOG_WARN("XSessionWriteStats: unknown session handle %p, writing anyway.", hSession);
	}
	if (xuid != xls::UserXuid(0)) {
		// Only the local Steam user can write to Steam so a host writing for its guests is ignored.
		XLS_LOG_DEBUG("XSessionWriteStats: skipping stats for remote user 0x%016llx.", xuid);
		return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
	}
	if (!xls::SteamReady()) {
		return xls::OverlappedReturn(pOverlapped, ERROR_NOT_LOGGED_ON);
	}
	bool statsTouched = false;
	for (DWORD v = 0; v < dwNumViews; v++) {
		const XSESSION_VIEW_PROPERTIES& view = pViews[v];
		if (view.dwViewId == X_STATS_VIEW_SKILL) {
			continue;
		}
		std::vector<uint16_t> requested;
		for (DWORD p = 0; p < view.dwNumProperties; p++) {
			requested.push_back((uint16_t)(view.pProperties[p].dwPropertyId & X_PROPERTY_ID_MASK));
		}
		std::vector<ColumnLayout> layout = LayoutFor(view.dwViewId, requested);
		auto find = [&](uint16_t columnId) -> const XUSER_PROPERTY* {
			for (DWORD p = 0; p < view.dwNumProperties; p++) {
				if ((view.pProperties[p].dwPropertyId & X_PROPERTY_ID_MASK) == columnId) {
					return &view.pProperties[p];
				}
			}
			return nullptr;
		};
		auto upload = std::make_shared<Upload>();
		upload->viewId = view.dwViewId;
		bool haveScore = false;
		for (const ColumnLayout& column : layout) {
			const XUSER_PROPERTY* property = find(column.columnId);
			if (column.isRating) {
				if (property) {
					upload->score = ScoreOf(property->value);
					haveScore = true;
				}
				continue;
			}
			XUSER_DATA zero = {};
			zero.type = column.type;
			EncodeDetail(property ? property->value : zero, column.type, upload->details);
		}
		if (!haveScore) {
			XLS_LOG_WARN("XSessionWriteStats: view %u has no value for its rating column, nothing uploaded.", view.dwViewId);
			continue;
		}
		StartUpload(upload);

		// Columns that the configuration also maps to Steam stats.
		for (DWORD p = 0; p < view.dwNumProperties; p++) {
			const XUSER_PROPERTY& property = view.pProperties[p];
			const std::string* statName = xls::StatNameFor(view.dwViewId, (uint16_t)(property.dwPropertyId & X_PROPERTY_ID_MASK));
			if (!statName) {
				continue;
			}
			if (property.value.type == XUSER_DATA_TYPE_FLOAT || property.value.type == XUSER_DATA_TYPE_DOUBLE) {
				float value = property.value.type == XUSER_DATA_TYPE_FLOAT ? property.value.fData : (float)property.value.dblData;
				xls::SteamUserStats()->SetStat(statName->c_str(), value);
			}
			else {
				xls::SteamUserStats()->SetStat(statName->c_str(), ScoreOf(property.value));
			}
			statsTouched = true;
		}
	}
	if (statsTouched) {
		xls::SteamUserStats()->StoreStats();
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5329
DWORD WINAPI XSessionFlushStats(HANDLE hSession, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (xls::SteamReady()) {
		xls::SteamUserStats()->StoreStats();
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5281
DWORD WINAPI XUserReadStats(DWORD dwTitleId, DWORD dwNumXuids, const XUID* pXuids, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbResults, XUSER_STATS_READ_RESULTS* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwNumXuids || dwNumXuids > X_STATS_MAX_USER_COUNT || !pXuids || !dwNumStatsSpecs || dwNumStatsSpecs > X_STATS_MAX_VIEWS || !pSpecs || !pcbResults) {
		return ERROR_INVALID_PARAMETER;
	}
	std::shared_ptr<ReadRequest> request = MakeRead(dwNumStatsSpecs, pSpecs);
	request->mode = ReadMode::Users;
	for (DWORD i = 0; i < dwNumXuids; i++) {
		CSteamID steamId = xls::SteamIdFromXuid(pXuids[i]);
		request->users.push_back(steamId.IsValid() ? steamId : CSteamID(pXuids[i] & 0xFFFFFFFF, k_EUniversePublic, k_EAccountTypeIndividual));
	}
	DWORD required = request->RequiredSize(dwNumXuids);
	if (!pResults || *pcbResults < required) {
		*pcbResults = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	DWORD bufferSize = *pcbResults;
	return xls::RunAsync(pOverlapped, [request, pResults, bufferSize](XOVERLAPPED* overlapped) {
		if (xls::SteamReady() && !request->Poll()) {
			return false;
		}
		if (!xls::SteamReady()) {
			for (ReadView& view : request->views) {
				request->FinishEmpty(view);
			}
		}
		bool packed = request->Pack(pResults, bufferSize);
		xls::OverlappedComplete(overlapped, packed ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER);
		return true;
	});
}

// #5284
DWORD WINAPI XUserCreateStatsEnumeratorByRank(DWORD dwTitleId, DWORD dwRankStart, DWORD dwNumRows, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!dwRankStart || !dwNumRows || dwNumRows > X_STATS_MAX_ROW_COUNT || !dwNumStatsSpecs || dwNumStatsSpecs > X_STATS_MAX_VIEWS || !pSpecs || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	std::shared_ptr<ReadRequest> request = MakeRead(dwNumStatsSpecs, pSpecs);
	request->mode = ReadMode::GlobalRange;
	request->rangeStart = (int)dwRankStart;
	request->rangeEnd = (int)(dwRankStart + dwNumRows - 1);
	return CreateStatsEnumerator(request, dwNumRows, pcbBuffer, phEnum);
}

// #5285
DWORD WINAPI XUserCreateStatsEnumeratorByRating(DWORD dwTitleId, LONGLONG i64Rating, DWORD dwNumRows, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!dwNumRows || dwNumRows > X_STATS_MAX_ROW_COUNT || !dwNumStatsSpecs || dwNumStatsSpecs > X_STATS_MAX_VIEWS || !pSpecs || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	// Steam cannot seek a leaderboard by score so the top of the board is returned instead.
	std::shared_ptr<ReadRequest> request = MakeRead(dwNumStatsSpecs, pSpecs);
	request->mode = ReadMode::GlobalRange;
	request->rangeStart = 1;
	request->rangeEnd = (int)dwNumRows;
	return CreateStatsEnumerator(request, dwNumRows, pcbBuffer, phEnum);
}

// #5286
DWORD WINAPI XUserCreateStatsEnumeratorByXuid(DWORD dwTitleId, XUID xuidPivot, DWORD dwNumRows, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xuidPivot || !dwNumRows || dwNumRows > X_STATS_MAX_ROW_COUNT || !dwNumStatsSpecs || dwNumStatsSpecs > X_STATS_MAX_VIEWS || !pSpecs || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	std::shared_ptr<ReadRequest> request = MakeRead(dwNumStatsSpecs, pSpecs);
	if (xuidPivot == xls::UserXuid(0)) {
		request->mode = ReadMode::AroundUser;
		request->rangeStart = -(int)(dwNumRows / 2);
		request->rangeEnd = (int)(dwNumRows - dwNumRows / 2 - 1);
	}
	else {
		// Steam only centres a download on the current user
		request->mode = ReadMode::Users;
		request->users.push_back(xls::SteamIdFromXuid(xuidPivot));
	}
	return CreateStatsEnumerator(request, dwNumRows, pcbBuffer, phEnum);
}

// #5287
DWORD WINAPI XUserResetStatsView(DWORD dwUserIndex, DWORD dwViewId, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	// A client cannot remove its own Steam leaderboard entry.
	XLS_LOG_WARN("XUserResetStatsView: view %u cannot be reset from the client.", dwViewId);
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5291
DWORD WINAPI XUserResetStatsViewAllUsers(DWORD dwViewId, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	XLS_LOG_WARN("XUserResetStatsViewAllUsers: view %u cannot be reset from the client.", dwViewId);
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5342
DWORD WINAPI XSessionModifySkill(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwXuidCount || !pXuids) {
		return ERROR_INVALID_PARAMETER;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5343
DWORD WINAPI XSessionCalculateSkill(DWORD dwNumSkills, double* rgMu, double* rgSigma, double* pdAggregateMu, double* pdAggregateSigma)
{
	XLS_TRACE_FN();
	if (!dwNumSkills || !rgMu || !rgSigma || !pdAggregateMu || !pdAggregateSigma) {
		return ERROR_INVALID_PARAMETER;
	}
	double mu = 0;
	double variance = 0;
	for (DWORD i = 0; i < dwNumSkills; i++) {
		mu += rgMu[i];
		variance += rgSigma[i] * rgSigma[i];
	}
	*pdAggregateMu = mu / dwNumSkills;
	*pdAggregateSigma = std::sqrt(variance) / dwNumSkills;
	return ERROR_SUCCESS;
}

// #5346
DWORD WINAPI XUserEstimateRankForRating(DWORD dwNumRequests, const XUSER_RANK_REQUEST* pRequests, DWORD cbResults, XUSER_ESTIMATE_RANK_RESULTS* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwNumRequests || !pRequests || !pResults) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD required = sizeof(XUSER_ESTIMATE_RANK_RESULTS) + dwNumRequests * sizeof(DWORD);
	if (cbResults < required) {
		return ERROR_INSUFFICIENT_BUFFER;
	}
	// Steam has no rank-for-score query, so the answer is rank 1.
	pResults->dwNumRanks = dwNumRequests;
	pResults->pdwRanks = (DWORD*)((uint8_t*)pResults + sizeof(XUSER_ESTIMATE_RANK_RESULTS));
	for (DWORD i = 0; i < dwNumRequests; i++) {
		pResults->pdwRanks[i] = 1;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}
