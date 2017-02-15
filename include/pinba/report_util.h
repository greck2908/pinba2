#ifndef PINBA__REPORT_UTIL_H_
#define PINBA__REPORT_UTIL_H_

#include <string>
#include <vector>
#include <utility>

#include <sparsehash/dense_hash_map>
#include <sparsehash/sparse_hash_map>

#include <meow/intrusive_ptr.hpp>
#include <meow/hash/hash.hpp>
#include <meow/hash/hash_impl.hpp>
#include <meow/format/format_to_string.hpp>

#include "pinba/globals.h"
#include "pinba/dictionary.h"
#include "pinba/report_key.h"

////////////////////////////////////////////////////////////////////////////////////////////////

struct report_key__hasher_t
{
	template<size_t N>
	inline constexpr size_t operator()(report_key_base_t<N> const& key) const
	{
		// TODO: try a "better" hash function here, like https://github.com/leo-yuriev/t1ha
		return meow::hash_blob(key.data(), key.size() * sizeof(typename report_key_base_t<N>::value_type));
	}
};

struct report_key__equal_t
{
	template<size_t N>
	inline bool operator()(report_key_base_t<N> const& l, report_key_base_t<N> const& r) const
	{
		return (l.size() == r.size())
				? (0 == memcmp(l.data(), r.data(), (l.size() * sizeof(typename report_key_base_t<N>::value_type)) ))
				: false
				;
	}
};

template<size_t N>
std::string report_key_to_string(report_key_base_t<N> const& k)
{
	std::string result;

	for (size_t i = 0; i < k.size(); ++i)
	{
		ff::write(result, (i == 0) ? "" : "|", k[i]);
	}

	return result;
}

template<size_t N>
std::string report_key_to_string(report_key_base_t<N> const& k, dictionary_t const *dict)
{
	std::string result;

	for (size_t i = 0; i < k.size(); ++i)
	{
		ff::fmt(result, "{0}{1}<{2}>", (i == 0) ? "" : "|", k[i], dict->get_word(k[i]));
	}

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////

/*
struct report_snapshot_traits___example
{
	using src_ticks_t = ; // source ticks_ringbuffer_t
	using hashtable_t = ; // result hashtable (that we're going to iterate over)

	// merge full tick from src ringbuffer to current hashtable_t state
	// from can be NULL (when there is no timeslice for this interval)
	static void merge_from_to(report_info_t& rinfo, typename src_ticks_t::value_t const *from, hashtable_t& to);
};
*/

template<class Traits>
struct report_snapshot__impl_t : public report_snapshot_t
{
	using src_ticks_t = typename Traits::src_ticks_t;
	using hashtable_t = typename Traits::hashtable_t;
	using iterator_t  = typename hashtable_t::iterator;

public:

	report_snapshot__impl_t(src_ticks_t const& ticks, report_info_t const& rinfo, dictionary_t *d)
		: data_()
		, ticks_(ticks)
		, rinfo_(rinfo)
		, dictionary_(d)
	{
	}

private:

	virtual report_info_t const* report_info() const override
	{
		return &rinfo_;
	}

	virtual dictionary_t const* dictionary() const override
	{
		return dictionary_;
	}

	virtual void prepare() override
	{
		if (this->is_prepared())
			return;

		for (auto& tick : ticks_)
		{
			// perform merge
			Traits::merge_from_to(rinfo_, tick.get(), data_);

			// release ref counted pointer as soon as we're done merging a tick
			// this allows to reduce memory footprint when merging in other thread
			// since report is working and ticks are being erased from it
			// (but this merge is holding them alive still)
			tick.reset();
		}

		ticks_.clear();
	}

	virtual bool is_prepared() const override
	{
		return ticks_.empty();
	}

private:

	union real_position_t
	{
		static_assert(sizeof(iterator_t) <= sizeof(position_t), "position_t must be able to hold iterator contents");

		real_position_t(iterator_t i) : it(i) {}
		real_position_t(position_t p) : pos(p) {}

		position_t pos;
		iterator_t it;
	};

	position_t position_from_iterator(iterator_t const& it)
	{
		real_position_t p { it };
		return p.pos;
	}

private:

	virtual position_t pos_first() override
	{
		return position_from_iterator(data_.begin());
	}

	virtual position_t pos_last() override
	{
		return position_from_iterator(data_.end());
	}

	virtual position_t pos_next(position_t const& pos) override
	{
		auto const& it = reinterpret_cast<iterator_t const&>(pos);
		return position_from_iterator(std::next(it));
	}

	virtual position_t pos_prev(position_t const& pos) override
	{
		auto const& it = reinterpret_cast<iterator_t const&>(pos);
		return position_from_iterator(std::prev(it));
	}

	virtual bool pos_equal(position_t const& l, position_t const& r) const override
	{
		auto const& l_it = reinterpret_cast<iterator_t const&>(l);
		auto const& r_it = reinterpret_cast<iterator_t const&>(r);
		return (l_it == r_it);
	}

	virtual report_key_t get_key(position_t const& pos) const override
	{
		auto const& it = reinterpret_cast<iterator_t const&>(pos);
		return it->first;
	}

	virtual report_key_str_t get_key_str(position_t const& pos) const override
	{
		report_key_t k = this->get_key(pos);

		report_key_str_t result;
		for (uint32_t i = 0; i < k.size(); ++i)
		{
			result.push_back(dictionary_->get_word(k[i]));
		}
		return result;
	}

	virtual int data_kind() const override
	{
		return rinfo_.kind;
	}

	virtual void* get_data(position_t const& pos) override
	{
		auto const& it = reinterpret_cast<iterator_t const&>(pos);
		return (void*)&it->second; // FIXME: const
	}

	virtual histogram_t const* get_histogram(position_t const& pos) override
	{
		if (!rinfo_.hv_enabled)
			return NULL;

		auto const& it = reinterpret_cast<iterator_t const&>(pos);
		return &it->second.hv;
	}

private:
	hashtable_t    data_;         // real data we iterate over
	src_ticks_t    ticks_;        // ticks we merge our data from (in other thread potentially)
	report_info_t  rinfo_;        // report info, immutable copy taken in ctor
	dictionary_t   *dictionary_;  // dictionary used to transform string ids to their values
};

////////////////////////////////////////////////////////////////////////////////////////////////

template<class T>
struct ticks_ringbuffer_t : private boost::noncopyable
{
	using data_t = T;

	struct tick_t
		: boost::intrusive_ref_counter<tick_t>
	{
		timeval_t  start_tv;
		timeval_t  end_tv;
		data_t     data;

		tick_t(timeval_t curr_tv)
			: start_tv(curr_tv)
			, end_tv({0,0})
			, data()
		{
		}
	};

	using tick_ptr     = boost::intrusive_ptr<tick_t> ;
	using ringbuffer_t = std::vector<tick_ptr>;

public:

	ticks_ringbuffer_t(uint32_t tick_count)
		: tick_count_(tick_count)
	{
	}

	void init(timeval_t curr_tv)
	{
		for (uint32_t i = 0; i < tick_count_; i++)
			ticks_.push_back({});

		curr_tick_.reset(new tick_t(curr_tv));
	}

	void tick(timeval_t curr_tv)
	{
		curr_tick_->end_tv = curr_tv;           // finish current tick

		ticks_.push_back(curr_tick_);           // copy tick to history
		curr_tick_.reset(new tick_t(curr_tv));  // and create new tick at 'current'

		// maybe truncate history, if it has grown too long
		if (ticks_.size() >  tick_count_)
		{
			ticks_.erase(ticks_.begin()); // XXX: O(N)
		}
	}

	ringbuffer_t const& get_internal_buffer() const
	{
		return ticks_;
	}

	tick_t& current()
	{
		return *curr_tick_;
	}

private:
	uint32_t      tick_count_;
	ringbuffer_t  ticks_;
	tick_ptr      curr_tick_;
};

////////////////////////////////////////////////////////////////////////////////////////////////

#endif // PINBA__REPORT_UTIL_H_