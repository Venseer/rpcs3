#include "stdafx.h"
#include "utils.h"
#include "aes.h"
#include "sha1.h"
#include "key_vault.h"
#include "Utilities/StrFmt.h"
#include "unpkg.h"

bool pkg_install(const fs::file& pkg_f, const std::string& dir, atomic_t<double>& sync, const std::string& pkg_filepath)
{
	const std::size_t BUF_SIZE = 8192 * 1024; // 8 MB
	std::vector<fs::file> filelist;
	u32 cur_file=0;

	fs::file original_file(pkg_filepath);
	original_file.seek(pkg_f.pos());

	filelist.push_back(std::move(original_file));

	// Save current file offset (probably zero)
	const u64 start_offset = pkg_f.pos();
	u64 cur_offset = start_offset;
	u64 cur_file_offset = start_offset;

	// Get basic PKG information
	PKGHeader header;

	auto archive_seek = [&](const s64 new_offset, const fs::seek_mode damode = fs::seek_set)
	{
		if(damode == fs::seek_set) cur_offset = new_offset;
		else if (damode == fs::seek_cur) cur_offset += new_offset;

		u64 _offset = 0;
		for (u32 i = 0; i < filelist.size(); i++)
		{
			if (cur_offset < (_offset + filelist[i].size()))
			{
				cur_file = i;
				cur_file_offset = cur_offset - _offset;
				filelist[i].seek(cur_file_offset);
				break;
			}
			_offset += filelist[i].size();
		}
	};

	auto archive_read = [&](const void *data_ptr, const u64 num_bytes)
	{
		u64 num_bytes_left = filelist[cur_file].size() - cur_file_offset;
		//check if it continues in another file
		if (num_bytes > num_bytes_left)
		{
			filelist[cur_file].read((u8 *)data_ptr, num_bytes_left);
			if ((cur_file + 1) < filelist.size()) cur_file++;
			else
			{
				cur_offset += num_bytes_left;
				cur_file_offset = filelist[cur_file].size();
				return num_bytes_left;
			}
			u64 num_read = filelist[cur_file].read((u8 *)data_ptr + num_bytes_left, num_bytes - num_bytes_left);
			cur_offset += (num_read + num_bytes_left);
			cur_file_offset = num_read;
			return (num_read+num_bytes_left);
		}

		u64 num_read = filelist[cur_file].read((u8 *)data_ptr, num_bytes);

		cur_offset += num_read;
		cur_file_offset += num_read;

		return num_read;
	};


	if (archive_read(&header, sizeof(header)) != sizeof(header))
	{
		LOG_ERROR(LOADER, "PKG file is too short!");
		return false;
	}

	if (header.pkg_magic != "\x7FPKG"_u32)
	{
		LOG_ERROR(LOADER, "Not a PKG file!");
		return false;
	}

	switch (const u16 type = header.pkg_type)
	{
	case PKG_RELEASE_TYPE_DEBUG:   break;
	case PKG_RELEASE_TYPE_RELEASE: break;
	default:
	{
		LOG_ERROR(LOADER, "Unknown PKG type (0x%x)", type);
		return false;
	}
	}

	switch (const u16 platform = header.pkg_platform)
	{
	case PKG_PLATFORM_TYPE_PS3: break;
	case PKG_PLATFORM_TYPE_PSP: break;
	default:
	{
		LOG_ERROR(LOADER, "Unknown PKG platform (0x%x)", platform);
		return false;
	}
	}

	if (header.pkg_size > pkg_f.size())
	{
		//Check if multi-files pkg
		if (pkg_filepath.length() < 7 || pkg_filepath.substr(pkg_filepath.length() - 7).compare("_00.pkg") != 0)
		{
			LOG_ERROR(LOADER, "PKG file size mismatch (pkg_size=0x%llx)", header.pkg_size);
			return false;
		}

		std::string name_wo_number = pkg_filepath.substr(0, pkg_filepath.length() - 7);
		u64 cursize = pkg_f.size();
		while (cursize != header.pkg_size)
		{
			std::string archive_filename = fmt::format("%s_%02d.pkg", name_wo_number, filelist.size());

			fs::file archive_file(archive_filename);
			if (!archive_file)
			{
				LOG_ERROR(LOADER, "Missing part of the multi-files pkg: %s", archive_filename);
				return false;
			}

			cursize += archive_file.size();
			filelist.push_back(std::move(archive_file));
		}
	}

	if (header.data_size + header.data_offset > header.pkg_size)
	{
		LOG_ERROR(LOADER, "PKG data size mismatch (data_size=0x%llx, data_offset=0x%llx, file_size=0x%llx)", header.data_size, header.data_offset, header.pkg_size);
		return false;
	}

	be_t<u32> drm_type{0};
	be_t<u32> content_type{0};

	archive_seek(header.pkg_info_off);

	for (u32 i = 0; i < header.pkg_info_num; i++)
	{
		struct packet_T
		{
			be_t<u32> id;
			be_t<u32> size;
		} packet;
		
		archive_read(&packet, sizeof(packet));

		// TODO
		switch (+packet.id)
		{
		case 0x1:
		{
			if (packet.size == sizeof(drm_type))
			{
				archive_read(&drm_type, sizeof(drm_type));
				continue;
			}

			break;
		}
		case 0x2:
		{
			if (packet.size == sizeof(content_type))
			{
				archive_read(&content_type, sizeof(content_type));
				continue;
			}

			break;
		}
		}

		archive_seek(packet.size, fs::seek_cur);
	}

	// Allocate buffer with BUF_SIZE size or more if required
	const std::unique_ptr<u128[]> buf(new u128[std::max<u64>(BUF_SIZE, sizeof(PKGEntry) * header.file_count) / sizeof(u128)]);

	// Define decryption subfunction (`psp` arg selects the key for specific block)
	auto decrypt = [&](u64 offset, u64 size, const uchar* key) -> u64
	{
		archive_seek(start_offset + header.data_offset + offset);

		// Read the data and set available size
		const u64 read = archive_read(buf.get(), size);

		// Get block count
		const u64 blocks = (read + 15) / 16;

		if (header.pkg_type == PKG_RELEASE_TYPE_DEBUG)
		{
			// Debug key
			be_t<u64> input[8] =
			{
				header.qa_digest[0],
				header.qa_digest[0],
				header.qa_digest[1],
				header.qa_digest[1],
			};

			for (u64 i = 0; i < blocks; i++)
			{
				// Initialize stream cipher for current position
				input[7] = offset / 16 + i;

				union sha1_hash
				{
					u8 data[20];
					u128 _v128;
				} hash;
				
				sha1(reinterpret_cast<const u8*>(input), sizeof(input), hash.data);

				buf[i] ^= hash._v128;
			}
		}

		if (header.pkg_type == PKG_RELEASE_TYPE_RELEASE)
		{
			aes_context ctx;

			// Set encryption key for stream cipher
			aes_setkey_enc(&ctx, key, 128);

			// Initialize stream cipher for start position
			be_t<u128> input = header.klicensee.value() + offset / 16;

			// Increment stream position for every block
			for (u64 i = 0; i < blocks; i++, input++)
			{
				u128 key;

				aes_crypt_ecb(&ctx, AES_ENCRYPT, reinterpret_cast<const u8*>(&input), reinterpret_cast<u8*>(&key));

				buf[i] ^= key;
			}
		}

		// Return the amount of data written in buf
		return read;
	};

	std::array<uchar, 16> dec_key;

	if (header.pkg_platform == PKG_PLATFORM_TYPE_PSP && content_type >= 0x15 && content_type <= 0x17)
	{
		const uchar psp2t1[] = {0xE3, 0x1A, 0x70, 0xC9, 0xCE, 0x1D, 0xD7, 0x2B, 0xF3, 0xC0, 0x62, 0x29, 0x63, 0xF2, 0xEC, 0xCB};
		const uchar psp2t2[] = {0x42, 0x3A, 0xCA, 0x3A, 0x2B, 0xD5, 0x64, 0x9F, 0x96, 0x86, 0xAB, 0xAD, 0x6F, 0xD8, 0x80, 0x1F};
		const uchar psp2t3[] = {0xAF, 0x07, 0xFD, 0x59, 0x65, 0x25, 0x27, 0xBA, 0xF1, 0x33, 0x89, 0x66, 0x8B, 0x17, 0xD9, 0xEA};

		aes_context ctx;
		aes_setkey_enc(&ctx, content_type == 0x15 ? psp2t1 : content_type == 0x16 ? psp2t2 : psp2t3, 128);
		aes_crypt_ecb(&ctx, AES_ENCRYPT, reinterpret_cast<const uchar*>(&header.klicensee), dec_key.data());
		decrypt(0, header.file_count * sizeof(PKGEntry), dec_key.data());
	}
	else
	{
		std::memcpy(dec_key.data(), PKG_AES_KEY, dec_key.size());
		decrypt(0, header.file_count * sizeof(PKGEntry), header.pkg_platform == PKG_PLATFORM_TYPE_PSP ? PKG_AES_KEY2 : dec_key.data());
	}

	std::vector<PKGEntry> entries(header.file_count);

	std::memcpy(entries.data(), buf.get(), entries.size() * sizeof(PKGEntry));

	for (const auto& entry : entries)
	{
		const bool is_psp = (entry.type & PKG_FILE_ENTRY_PSP) != 0;

		if (entry.name_size > 256)
		{
			LOG_ERROR(LOADER, "PKG name size is too big (0x%x)", entry.name_size);
			continue;
		}

		decrypt(entry.name_offset, entry.name_size, is_psp ? PKG_AES_KEY2 : dec_key.data());

		const std::string name(reinterpret_cast<char*>(buf.get()), entry.name_size);

		LOG_NOTICE(LOADER, "Entry 0x%08x: %s", entry.type, name);

		switch (entry.type & 0xff)
		{
		case PKG_FILE_ENTRY_NPDRM:
		case PKG_FILE_ENTRY_NPDRMEDAT:
		case PKG_FILE_ENTRY_SDAT:
		case PKG_FILE_ENTRY_REGULAR:
		case PKG_FILE_ENTRY_UNK1:
		case 0xe:
		case 0x10:
		case 0x11:
		case 0x13:
		case 0x15:
		case 0x16:
		{
			const std::string path = dir + name;

			const bool did_overwrite = fs::is_file(path);

			if (did_overwrite && (entry.type&PKG_FILE_ENTRY_OVERWRITE) == 0)
			{
				LOG_NOTICE(LOADER, "Didn't overwrite %s", name);
				break;
			}

			if (fs::file out{ path, fs::rewrite })
			{
				for (u64 pos = 0; pos < entry.file_size; pos += BUF_SIZE)
				{
					const u64 block_size = std::min<u64>(BUF_SIZE, entry.file_size - pos);

					if (decrypt(entry.file_offset + pos, block_size, is_psp ? PKG_AES_KEY2 : dec_key.data()) != block_size)
					{
						LOG_ERROR(LOADER, "Failed to extract file %s", path);
						break;
					}

					if (out.write(buf.get(), block_size) != block_size)
					{
						LOG_ERROR(LOADER, "Failed to write file %s", path);
						break;
					}

					if (sync.fetch_add((block_size + 0.0) / header.data_size) < 0.)
					{
						LOG_ERROR(LOADER, "Package installation cancelled: %s", dir);
						return false;
					}
				}

				if (did_overwrite)
				{
					LOG_WARNING(LOADER, "Overwritten file %s", name);
				}
				else
				{
					LOG_NOTICE(LOADER, "Created file %s", name);
				}
			}
			else
			{
				LOG_ERROR(LOADER, "Failed to create file %s", path);
			}

			break;
		}

		case PKG_FILE_ENTRY_FOLDER:
		case 0x12:
		{
			const std::string path = dir + name;

			if (fs::create_dir(path))
			{
				LOG_NOTICE(LOADER, "Created directory %s", name);
			}
			else if (fs::is_dir(path))
			{
				LOG_WARNING(LOADER, "Reused existing directory %s", name);
			}
			else
			{
				LOG_ERROR(LOADER, "Failed to create directory %s", path);
			}

			break;
		}

		default:
		{
			LOG_ERROR(LOADER, "Unknown PKG entry type (0x%x) %s", entry.type, name);
		}
		}
	}

	LOG_SUCCESS(LOADER, "Package successfully installed to %s", dir);
	return true;
}
