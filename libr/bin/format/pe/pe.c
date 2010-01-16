/* radare - LGPL - Copyright 2008 nibble<.ds@gmail.com> */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <r_types.h>

#include "pe.h"

static PE_DWord PE_(r_bin_pe_aux_rva_to_offset)(PE_(r_bin_pe_obj) *bin, PE_DWord rva)
{
	PE_DWord section_base;
	int i, section_size;

	for (i = 0; i < bin->nt_headers->file_header.NumberOfSections; i++) {
		section_base = bin->section_header[i].VirtualAddress;
		section_size = bin->section_header[i].Misc.VirtualSize;
		if (rva >= section_base && rva < section_base + section_size)
			return bin->section_header[i].PointerToRawData + (rva - section_base);
	}
		
	return 0;
}

#if 0
static PE_DWord PE_(r_bin_pe_aux_offset_to_rva)(PE_(r_bin_pe_obj) *bin, PE_DWord offset)
{
	PE_(image_section_header) *shdrp;
	PE_DWord section_base;
	int i, section_size;

	shdrp = bin->section_header;
	for (i = 0; i < bin->nt_headers->file_header.NumberOfSections; i++, shdrp++) {
		section_base = shdrp->PointerToRawData;
		section_size = shdrp->SizeOfRawData;
		if (offset >= section_base && offset < section_base + section_size)
			return shdrp->VirtualAddress + (offset - section_base);
	}
		
	return 0;
}
#endif

static int PE_(r_bin_pe_init)(PE_(r_bin_pe_obj) *bin)
{
	int sections_size, len;

	len = lseek(bin->fd, 0, SEEK_END);

	lseek(bin->fd, 0, SEEK_SET);
	bin->dos_header = malloc(sizeof(PE_(image_dos_header)));
	read(bin->fd, bin->dos_header, sizeof(PE_(image_dos_header)));

	if (bin->dos_header->e_lfanew > len) {
		ERR("Invalid e_lfanew field\n");
		return -1;
	}

	lseek(bin->fd, bin->dos_header->e_lfanew, SEEK_SET);
	bin->nt_headers = malloc(sizeof(PE_(image_nt_headers)));
	read(bin->fd, bin->nt_headers, sizeof(PE_(image_nt_headers)));

	if (strncmp((char*)&bin->dos_header->e_magic, "MZ", 2)) {
		ERR("File not PE\n");
		return -1;
	}

	if (strncmp((char*)&bin->nt_headers->Signature, "PE", 2)) {
		ERR("File not PE\n");
		return -1;
	}

	sections_size = sizeof(PE_(image_section_header)) * bin->nt_headers->file_header.NumberOfSections;

	if (sections_size > len) {
		ERR("Invalid NumberOfSections value\n");
		return -1;
	}

	lseek(bin->fd, bin->dos_header->e_lfanew + sizeof(PE_(image_nt_headers)), SEEK_SET);
	bin->section_header = malloc(sections_size);
	read(bin->fd, bin->section_header, sections_size);

	return 0;
}

static int PE_(r_bin_pe_init_exports)(PE_(r_bin_pe_obj) *bin)
{
	PE_(image_data_directory) *data_dir_export = &bin->nt_headers->optional_header.DataDirectory[PE_IMAGE_DIRECTORY_ENTRY_EXPORT];
	PE_DWord export_dir_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, data_dir_export->VirtualAddress);

	if (export_dir_offset == 0)
		return -1;

	lseek(bin->fd, export_dir_offset, SEEK_SET);
	bin->export_directory = malloc(sizeof(PE_(image_export_directory)));
	read(bin->fd, bin->export_directory, sizeof(PE_(image_export_directory)));

	return 0;
}

static int PE_(r_bin_pe_init_imports)(PE_(r_bin_pe_obj) *bin)
{
	PE_(image_data_directory) *data_dir_import = &bin->nt_headers->optional_header.DataDirectory[PE_IMAGE_DIRECTORY_ENTRY_IMPORT];
	PE_(image_data_directory) *data_dir_delay_import = &bin->nt_headers->optional_header.DataDirectory[PE_IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
	PE_DWord import_dir_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, data_dir_import->VirtualAddress);
	PE_DWord delay_import_dir_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, data_dir_delay_import->VirtualAddress);
	int import_dir_size = data_dir_import->Size;
	int delay_import_dir_size = data_dir_delay_import->Size;
	
	if (import_dir_offset == 0 && delay_import_dir_offset == 0)
		return -1;

	if (import_dir_offset != 0) {
		lseek(bin->fd, import_dir_offset, SEEK_SET);
		bin->import_directory = malloc(import_dir_size);
		read(bin->fd, bin->import_directory, import_dir_size);
	}

	if (delay_import_dir_offset != 0) {
		lseek(bin->fd, delay_import_dir_offset, SEEK_SET);
		bin->delay_import_directory = malloc(delay_import_dir_size);
		read(bin->fd, bin->delay_import_directory, delay_import_dir_size);
	}

	return 0;
}

static int PE_(r_bin_pe_get_import_dirs_count)(PE_(r_bin_pe_obj) *bin)
{
	PE_(image_data_directory) *data_dir_import = &bin->nt_headers->optional_header.DataDirectory[PE_IMAGE_DIRECTORY_ENTRY_IMPORT];

	return (int) (data_dir_import->Size / sizeof(PE_(image_import_directory)) - 1);
}

static int PE_(r_bin_pe_get_delay_import_dirs_count)(PE_(r_bin_pe_obj) *bin)
{
	PE_(image_data_directory) *data_dir_delay_import = \
		&bin->nt_headers->optional_header.DataDirectory[PE_IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];

	return (int) (data_dir_delay_import->Size / sizeof(PE_(image_delay_import_directory)) - 1);
}

static int PE_(r_bin_pe_parse_imports)(PE_(r_bin_pe_obj) *bin, PE_(r_bin_pe_import) **importp, char *dll_name, PE_DWord OriginalFirstThunk, PE_DWord FirstThunk)
{
	char import_name[PE_NAME_LENGTH], name[PE_NAME_LENGTH];
	PE_Word import_hint, import_ordinal;
	PE_DWord import_table = 0, off = 0;
	int i = 0;

	if ((off = PE_(r_bin_pe_aux_rva_to_offset)(bin, OriginalFirstThunk)) == 0 &&
		(off = PE_(r_bin_pe_aux_rva_to_offset)(bin, FirstThunk)) == 0)
		return 0;

	do {
		lseek(bin->fd, off + i * sizeof(PE_DWord), SEEK_SET);
		read(bin->fd, &import_table, sizeof(PE_DWord));
		if (import_table) {
			if (import_table & ILT_MASK1) {
				import_ordinal = import_table & ILT_MASK2;
				import_hint = 0;
				snprintf(import_name, PE_NAME_LENGTH, "%s_Ordinal_%i", dll_name, import_ordinal);
			} else {
				import_ordinal = 0;
				lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, import_table), SEEK_SET);
				read(bin->fd, &import_hint, sizeof(PE_Word));
				read(bin->fd, name, PE_NAME_LENGTH);
				snprintf(import_name, PE_NAME_LENGTH, "%s_%s", dll_name, name);
			}
			memcpy((*importp)->name, import_name, PE_NAME_LENGTH);
			(*importp)->name[PE_NAME_LENGTH-1] = '\0';
			(*importp)->rva = FirstThunk + i * sizeof(PE_DWord);
			(*importp)->offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, FirstThunk) + i * sizeof(PE_DWord);
			(*importp)->hint = import_hint;
			(*importp)->ordinal = import_ordinal;
			(*importp)++; i++;
		}
	} while (import_table);

	return i;
}

int PE_(r_bin_pe_close)(PE_(r_bin_pe_obj) *bin)
{
	if (bin->dos_header != NULL)
		free(bin->dos_header);
	if (bin->nt_headers != NULL)
		free(bin->nt_headers);
	if (bin->section_header != NULL)
		free(bin->section_header);
	if (bin->export_directory != NULL)
		free(bin->export_directory);
	if (bin->import_directory != NULL)
		free(bin->import_directory);
	if (bin->delay_import_directory != NULL)
		free(bin->delay_import_directory);

	return close(bin->fd);
}

int PE_(r_bin_pe_get_arch)(PE_(r_bin_pe_obj) *bin, char *str)
{
	if (str)
	switch (bin->nt_headers->file_header.Machine) {
	case PE_IMAGE_FILE_MACHINE_ALPHA:
	case PE_IMAGE_FILE_MACHINE_ALPHA64:
		snprintf(str, PE_NAME_LENGTH, "alpha");
		break;
	case PE_IMAGE_FILE_MACHINE_ARM:
	case PE_IMAGE_FILE_MACHINE_THUMB:
		snprintf(str, PE_NAME_LENGTH, "arm");
		break;
	case PE_IMAGE_FILE_MACHINE_M68K:
		snprintf(str, PE_NAME_LENGTH, "m68k");
		break;
	case PE_IMAGE_FILE_MACHINE_MIPS16:
	case PE_IMAGE_FILE_MACHINE_MIPSFPU:
	case PE_IMAGE_FILE_MACHINE_MIPSFPU16:
	case PE_IMAGE_FILE_MACHINE_WCEMIPSV2:
		snprintf(str, PE_NAME_LENGTH, "mips");
		break;
	case PE_IMAGE_FILE_MACHINE_POWERPC:
	case PE_IMAGE_FILE_MACHINE_POWERPCFP:
		snprintf(str, PE_NAME_LENGTH, "ppc");
		break;
	case PE_IMAGE_FILE_MACHINE_AMD64:
	case PE_IMAGE_FILE_MACHINE_IA64:
		snprintf(str, PE_NAME_LENGTH, "intel64");
		break;
	default:
		snprintf(str, PE_NAME_LENGTH, "intel");
	}
	return bin->nt_headers->file_header.Machine;
}

int PE_(r_bin_pe_get_entrypoint)(PE_(r_bin_pe_obj) *bin, PE_(r_bin_pe_entrypoint) *entrypoint)
{
	entrypoint->rva = bin->nt_headers->optional_header.AddressOfEntryPoint;
	entrypoint->offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, bin->nt_headers->optional_header.AddressOfEntryPoint);
	return 0;
}

int PE_(r_bin_pe_get_exports)(PE_(r_bin_pe_obj) *bin, PE_(r_bin_pe_export) *export)
{
	PE_CWord functions_offset, names_offset, ordinals_offset, function_rva, name_rva, name_offset;
	PE_Word function_ordinal;
	PE_(r_bin_pe_export) *exportp;
	char function_name[PE_NAME_LENGTH], forwarder_name[PE_NAME_LENGTH];
	char dll_name[PE_NAME_LENGTH], export_name[PE_NAME_LENGTH];
	int i;
	PE_(image_data_directory) *data_dir_export =
		&bin->nt_headers->optional_header.DataDirectory[PE_IMAGE_DIRECTORY_ENTRY_EXPORT];
	PE_CWord export_dir_rva = data_dir_export->VirtualAddress;
	int export_dir_size = data_dir_export->Size;
	
	if (PE_(r_bin_pe_init_exports)(bin) == -1)
		return -1;
	
	lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, bin->export_directory->Name), SEEK_SET);
    	read(bin->fd, dll_name, PE_NAME_LENGTH);

	functions_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, bin->export_directory->AddressOfFunctions);
	names_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, bin->export_directory->AddressOfNames);
	ordinals_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, bin->export_directory->AddressOfOrdinals);

	exportp = export;
	for (i = 0; i < bin->export_directory->NumberOfNames; i++, exportp++) {
		lseek(bin->fd, functions_offset + i * sizeof(PE_CWord), SEEK_SET);
		read(bin->fd, &function_rva, sizeof(PE_CWord));
		lseek(bin->fd, ordinals_offset + i * sizeof(PE_Word), SEEK_SET);
		read(bin->fd, &function_ordinal, sizeof(PE_Word));
		lseek(bin->fd, names_offset + i * sizeof(PE_CWord), SEEK_SET);
		read(bin->fd, &name_rva, sizeof(PE_CWord));
		name_offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, name_rva);

		if (name_offset) {
			lseek(bin->fd, name_offset, SEEK_SET);
			read(bin->fd, function_name, PE_NAME_LENGTH);
		} else {
			snprintf(function_name, PE_NAME_LENGTH, "Ordinal_%i", function_ordinal);
		}
		
		snprintf(export_name, PE_NAME_LENGTH, "%s_%s", dll_name, function_name);

		if (function_rva >= export_dir_rva && function_rva < (export_dir_rva + export_dir_size)) {
			lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, function_rva), SEEK_SET);
			read(bin->fd, forwarder_name, PE_NAME_LENGTH);
		} else {
			snprintf(forwarder_name, PE_NAME_LENGTH, "NONE");
		}

		exportp->rva = function_rva;
		exportp->offset = PE_(r_bin_pe_aux_rva_to_offset)(bin, function_rva);
		exportp->ordinal = function_ordinal;
		memcpy(exportp->forwarder, forwarder_name, PE_NAME_LENGTH);
		exportp->forwarder[PE_NAME_LENGTH-1] = '\0';
		memcpy(exportp->name, export_name, PE_NAME_LENGTH);
		exportp->name[PE_NAME_LENGTH-1] = '\0';
	}

	return 0;
}

int PE_(r_bin_pe_get_exports_count)(PE_(r_bin_pe_obj) *bin)
{
	if (PE_(r_bin_pe_init_exports)(bin) == -1)
		return 0;
	
	return bin->export_directory->NumberOfNames;
}

int PE_(r_bin_pe_get_file_alignment)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->optional_header.FileAlignment;
}

ut64 PE_(r_bin_pe_get_image_base)(PE_(r_bin_pe_obj) *bin)
{
	return(ut64)bin->nt_headers->optional_header.ImageBase;
}

int PE_(r_bin_pe_get_imports)(PE_(r_bin_pe_obj) *bin, PE_(r_bin_pe_import) *import)
{
	PE_(image_import_directory) *import_dirp;
	PE_(image_delay_import_directory) *delay_import_dirp;
	PE_(r_bin_pe_import) *importp;
	char dll_name[PE_NAME_LENGTH];
	int import_dirs_count = PE_(r_bin_pe_get_import_dirs_count)(bin);
	int delay_import_dirs_count = PE_(r_bin_pe_get_delay_import_dirs_count)(bin);
	int i;
	
	if (PE_(r_bin_pe_init_imports)(bin) == -1)
		return -1;

	importp = import;

	import_dirp = bin->import_directory;
	for (i = 0; i < import_dirs_count; i++, import_dirp++) {
		lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, import_dirp->Name), SEEK_SET);
		read(bin->fd, dll_name, PE_NAME_LENGTH);
		if (!PE_(r_bin_pe_parse_imports)(bin, &importp, dll_name,
			import_dirp->Characteristics, import_dirp->FirstThunk))
			break;
	}

	delay_import_dirp = bin->delay_import_directory;
	for (i = 0; i < delay_import_dirs_count; i++, delay_import_dirp++) {
		lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, delay_import_dirp->Name), SEEK_SET);
		read(bin->fd, dll_name, PE_NAME_LENGTH);
		if(!PE_(r_bin_pe_parse_imports)(bin, &importp, dll_name, delay_import_dirp->DelayImportNameTable, delay_import_dirp->DelayImportAddressTable))
			break;
	}

	return 0;
}

int PE_(r_bin_pe_get_imports_count)(PE_(r_bin_pe_obj) *bin)
{
	PE_(image_import_directory) *import_dirp;
	PE_(image_delay_import_directory) *delay_import_dirp;
	PE_DWord import_table, off = 0;
	int import_dirs_count = PE_(r_bin_pe_get_import_dirs_count)(bin);
	int delay_import_dirs_count = PE_(r_bin_pe_get_delay_import_dirs_count)(bin);
	int imports_count = 0, i, j;

	if (PE_(r_bin_pe_init_imports)(bin) == -1)
		return 0;

	import_dirp = bin->import_directory;
	import_table = 0;
	for (i = 0; i < import_dirs_count; i++, import_dirp++) {
		if ((off = PE_(r_bin_pe_aux_rva_to_offset)(bin, import_dirp->Characteristics)) == 0 &&
			(off = PE_(r_bin_pe_aux_rva_to_offset)(bin, import_dirp->FirstThunk)) == 0)
			break;

		j = 0;
		do {
			lseek(bin->fd, off + j * sizeof(PE_DWord), SEEK_SET);
			read(bin->fd, &import_table, sizeof(PE_DWord));

			if (import_table) {
				imports_count++;
				j++;
			}
		} while (import_table);
	}

	delay_import_dirp = bin->delay_import_directory;
	import_table = 0;
	for (i = 0; i < delay_import_dirs_count; i++, delay_import_dirp++) {
		if (delay_import_dirp->DelayImportNameTable)
			off = PE_(r_bin_pe_aux_rva_to_offset)(bin, delay_import_dirp->DelayImportNameTable);
		else if (delay_import_dirp->DelayImportAddressTable) 
			off = PE_(r_bin_pe_aux_rva_to_offset)(bin, delay_import_dirp->DelayImportNameTable);
		else break;

		j = 0;
		do {
			lseek(bin->fd, off + j * sizeof(PE_DWord), SEEK_SET);
    		read(bin->fd, &import_table, sizeof(PE_DWord));
			
			if (import_table) {
				imports_count++;
				j++;
			}
		} while (import_table);
	}

	return imports_count;
}

int PE_(r_bin_pe_get_libs)(PE_(r_bin_pe_obj) *bin, int limit, PE_(r_bin_pe_string) *strings)
{
	PE_(image_import_directory) *import_dirp;
	PE_(image_delay_import_directory) *delay_import_dirp;
	PE_(r_bin_pe_string) *stringsp;
	char dll_name[PE_STRING_LENGTH];
	int import_dirs_count = PE_(r_bin_pe_get_import_dirs_count)(bin), delay_import_dirs_count = PE_(r_bin_pe_get_delay_import_dirs_count)(bin);
	int i, ctr=0;
	
	if (PE_(r_bin_pe_init_imports)(bin) == -1)
		return -1;

	import_dirp = bin->import_directory;
	stringsp = strings;
	for (i = 0; i < import_dirs_count && ctr < limit; i++, import_dirp++, stringsp++) {
		lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, import_dirp->Name), SEEK_SET);
		read(bin->fd, dll_name, PE_STRING_LENGTH);
		memcpy(stringsp->string, dll_name, PE_STRING_LENGTH);
		stringsp->string[PE_STRING_LENGTH-1] = '\0';
		stringsp->type = 'A';
		stringsp->offset = 0;
		stringsp->size = 0;
		ctr++;
	}
	
	delay_import_dirp = bin->delay_import_directory;
	for (i = 0; i < delay_import_dirs_count && ctr < limit; i++, delay_import_dirp++, stringsp++) {
		lseek(bin->fd, PE_(r_bin_pe_aux_rva_to_offset)(bin, delay_import_dirp->Name), SEEK_SET);
		read(bin->fd, dll_name, PE_STRING_LENGTH);
		memcpy(stringsp->string, dll_name, PE_STRING_LENGTH);
		stringsp->string[PE_STRING_LENGTH-1] = '\0';
		stringsp->type = 'A';
		stringsp->offset = 0;
		stringsp->size = 0;
		ctr++;
	}
	
	return ctr;
}

int PE_(r_bin_pe_get_image_size)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->optional_header.SizeOfImage;
}

int PE_(r_bin_pe_get_machine)(PE_(r_bin_pe_obj) *bin, char *str)
{
	if (str)
	switch (bin->nt_headers->file_header.Machine) {
	case PE_IMAGE_FILE_MACHINE_ALPHA:
		snprintf(str, PE_NAME_LENGTH, "Alpha");
		break;
	case PE_IMAGE_FILE_MACHINE_ALPHA64:
		snprintf(str, PE_NAME_LENGTH, "Alpha 64");
		break;
	case PE_IMAGE_FILE_MACHINE_AM33:
		snprintf(str, PE_NAME_LENGTH, "AM33");
		break;
	case PE_IMAGE_FILE_MACHINE_AMD64:
		snprintf(str, PE_NAME_LENGTH, "AMD 64");
		break;
	case PE_IMAGE_FILE_MACHINE_ARM:
		snprintf(str, PE_NAME_LENGTH, "ARM");
		break;
	case PE_IMAGE_FILE_MACHINE_CEE:
		snprintf(str, PE_NAME_LENGTH, "CEE");
		break;
	case PE_IMAGE_FILE_MACHINE_CEF:
		snprintf(str, PE_NAME_LENGTH, "CEF");
		break;
	case PE_IMAGE_FILE_MACHINE_EBC:
		snprintf(str, PE_NAME_LENGTH, "EBC");
		break;
	case PE_IMAGE_FILE_MACHINE_I386:
		snprintf(str, PE_NAME_LENGTH, "i386");
		break;
	case PE_IMAGE_FILE_MACHINE_IA64:
		snprintf(str, PE_NAME_LENGTH, "ia64");
		break;
	case PE_IMAGE_FILE_MACHINE_M32R:
		snprintf(str, PE_NAME_LENGTH, "M32R");
		break;
	case PE_IMAGE_FILE_MACHINE_M68K:
		snprintf(str, PE_NAME_LENGTH, "M68K");
		break;
	case PE_IMAGE_FILE_MACHINE_MIPS16:
		snprintf(str, PE_NAME_LENGTH, "Mips 16");
		break;
	case PE_IMAGE_FILE_MACHINE_MIPSFPU:
		snprintf(str, PE_NAME_LENGTH, "Mips FPU");
		break;
	case PE_IMAGE_FILE_MACHINE_MIPSFPU16:
		snprintf(str, PE_NAME_LENGTH, "Mips FPU 16");
		break;
	case PE_IMAGE_FILE_MACHINE_POWERPC:
		snprintf(str, PE_NAME_LENGTH, "PowerPC");
		break;
	case PE_IMAGE_FILE_MACHINE_POWERPCFP:
		snprintf(str, PE_NAME_LENGTH, "PowerPC FP");
		break;
	case PE_IMAGE_FILE_MACHINE_R10000:
		snprintf(str, PE_NAME_LENGTH, "R10000");
		break;
	case PE_IMAGE_FILE_MACHINE_R3000:
		snprintf(str, PE_NAME_LENGTH, "R3000");
		break;
	case PE_IMAGE_FILE_MACHINE_R4000:
		snprintf(str, PE_NAME_LENGTH, "R4000");
		break;
	case PE_IMAGE_FILE_MACHINE_SH3:
		snprintf(str, PE_NAME_LENGTH, "SH3");
		break;
	case PE_IMAGE_FILE_MACHINE_SH3DSP:
		snprintf(str, PE_NAME_LENGTH, "SH3DSP");
		break;
	case PE_IMAGE_FILE_MACHINE_SH3E:
		snprintf(str, PE_NAME_LENGTH, "SH3E");
		break;
	case PE_IMAGE_FILE_MACHINE_SH4:
		snprintf(str, PE_NAME_LENGTH, "SH4");
		break;
	case PE_IMAGE_FILE_MACHINE_SH5:
		snprintf(str, PE_NAME_LENGTH, "SH5");
		break;
	case PE_IMAGE_FILE_MACHINE_THUMB:
		snprintf(str, PE_NAME_LENGTH, "Thumb");
		break;
	case PE_IMAGE_FILE_MACHINE_TRICORE:
		snprintf(str, PE_NAME_LENGTH, "Tricore");
		break;
	case PE_IMAGE_FILE_MACHINE_WCEMIPSV2:
		snprintf(str, PE_NAME_LENGTH, "WCE Mips V2");
		break;
	default:
		snprintf(str, PE_NAME_LENGTH, "unknown");
	}

	return bin->nt_headers->file_header.Machine;
}

int PE_(r_bin_pe_get_os)(PE_(r_bin_pe_obj) *bin, char *str)
{
	if (str)
	switch (bin->nt_headers->optional_header.Subsystem) {
	case PE_IMAGE_SUBSYSTEM_NATIVE:
		snprintf(str, PE_NAME_LENGTH, "native");
		break;
	case PE_IMAGE_SUBSYSTEM_WINDOWS_GUI:
	case PE_IMAGE_SUBSYSTEM_WINDOWS_CUI:
	case PE_IMAGE_SUBSYSTEM_WINDOWS_CE_GUI:
		snprintf(str, PE_NAME_LENGTH, "windows");
		break;
	case PE_IMAGE_SUBSYSTEM_POSIX_CUI:
		snprintf(str, PE_NAME_LENGTH, "posix");
		break;
	case PE_IMAGE_SUBSYSTEM_EFI_APPLICATION:
	case PE_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
	case PE_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
	case PE_IMAGE_SUBSYSTEM_EFI_ROM:
		snprintf(str, PE_NAME_LENGTH, "efi");
		break;
	case PE_IMAGE_SUBSYSTEM_XBOX:
		snprintf(str, PE_NAME_LENGTH, "xbox");
		break;
	default:
		snprintf(str, PE_NAME_LENGTH, "unknown");
	}

	return bin->nt_headers->optional_header.Subsystem;
}

int PE_(r_bin_pe_get_class)(PE_(r_bin_pe_obj) *bin, char *str)
{
	if (str)
	switch (bin->nt_headers->optional_header.Magic) {
	case PE_IMAGE_FILE_TYPE_PE32:
		snprintf(str, PE_NAME_LENGTH, "PE32");
		break;
	case PE_IMAGE_FILE_TYPE_PE32PLUS:
		snprintf(str, PE_NAME_LENGTH, "PE32+");
		break;
	}
	return bin->nt_headers->optional_header.Magic;
}

int PE_(r_bin_pe_get_section_alignment)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->optional_header.SectionAlignment;
}

int PE_(r_bin_pe_get_sections)(PE_(r_bin_pe_obj) *bin, PE_(r_bin_pe_section) *section)
{
	PE_(image_section_header) *shdrp;
	PE_(r_bin_pe_section) *sectionp;
	int i, sections_count = PE_(r_bin_pe_get_sections_count)(bin);

	shdrp = bin->section_header;
	sectionp = section;
	for (i = 0; i < sections_count; i++, shdrp++, sectionp++) {
		memcpy(sectionp->name, shdrp->Name, PE_IMAGE_SIZEOF_SHORT_NAME);
		sectionp->name[PE_IMAGE_SIZEOF_SHORT_NAME-1] = '\0';
		sectionp->rva = shdrp->VirtualAddress;
		sectionp->size = shdrp->SizeOfRawData;
		sectionp->vsize = shdrp->Misc.VirtualSize;
		sectionp->offset = shdrp->PointerToRawData;
		sectionp->characteristics = shdrp->Characteristics;
	}

	return 0;
}

int PE_(r_bin_pe_get_sections_count)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->file_header.NumberOfSections;
}

int PE_(r_bin_pe_get_subsystem)(PE_(r_bin_pe_obj) *bin, char *str)
{
	if (str)
	switch (bin->nt_headers->optional_header.Subsystem) {
	case PE_IMAGE_SUBSYSTEM_UNKNOWN:
		snprintf(str, PE_NAME_LENGTH, "Unknown");
		break;
	case PE_IMAGE_SUBSYSTEM_NATIVE:
		snprintf(str, PE_NAME_LENGTH, "Native");
		break;
	case PE_IMAGE_SUBSYSTEM_WINDOWS_GUI:
		snprintf(str, PE_NAME_LENGTH, "Windows GUI");
		break;
	case PE_IMAGE_SUBSYSTEM_WINDOWS_CUI:
		snprintf(str, PE_NAME_LENGTH, "Windows CUI");
		break;
	case PE_IMAGE_SUBSYSTEM_POSIX_CUI:
		snprintf(str, PE_NAME_LENGTH, "POSIX CUI");
		break;
	case PE_IMAGE_SUBSYSTEM_WINDOWS_CE_GUI:
		snprintf(str, PE_NAME_LENGTH, "Windows CE GUI");
		break;
	case PE_IMAGE_SUBSYSTEM_EFI_APPLICATION:
		snprintf(str, PE_NAME_LENGTH, "EFI Application");
		break;
	case PE_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
		snprintf(str, PE_NAME_LENGTH, "EFI Boot Service Driver");
		break;
	case PE_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
		snprintf(str, PE_NAME_LENGTH, "EFI Runtime Driver");
		break;
	case PE_IMAGE_SUBSYSTEM_EFI_ROM:
		snprintf(str, PE_NAME_LENGTH, "EFI ROM");
		break;
	case PE_IMAGE_SUBSYSTEM_XBOX:
		snprintf(str, PE_NAME_LENGTH, "XBOX");
		break;
	}
	return bin->nt_headers->optional_header.Subsystem;
}

int PE_(r_bin_pe_is_dll)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->file_header.Characteristics & PE_IMAGE_FILE_DLL;
}

int PE_(r_bin_pe_is_big_endian)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->file_header.Characteristics & PE_IMAGE_FILE_BYTES_REVERSED_HI;
}

int PE_(r_bin_pe_is_stripped_relocs)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->file_header.Characteristics & PE_IMAGE_FILE_RELOCS_STRIPPED;
}

int PE_(r_bin_pe_is_stripped_line_nums)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->file_header.Characteristics & PE_IMAGE_FILE_LINE_NUMS_STRIPPED;
}

int PE_(r_bin_pe_is_stripped_local_syms)(PE_(r_bin_pe_obj) *bin) {
	return bin->nt_headers->file_header.Characteristics & PE_IMAGE_FILE_LOCAL_SYMS_STRIPPED;
}

int PE_(r_bin_pe_is_stripped_debug)(PE_(r_bin_pe_obj) *bin)
{
	return bin->nt_headers->file_header.Characteristics & PE_IMAGE_FILE_DEBUG_STRIPPED;
}

int PE_(r_bin_pe_open)(PE_(r_bin_pe_obj) *bin, const char *file)
{
	bin->dos_header = NULL;
	bin->nt_headers = NULL;
	bin->section_header = NULL;
	bin->export_directory = NULL;
	bin->import_directory = NULL;
	bin->delay_import_directory = NULL;

	if ((bin->fd = open(file, O_RDONLY)) == -1)
		return -1;

	bin->file = file;

	if (PE_(r_bin_pe_init)(bin) == -1) {
		close(bin->fd);
		return -1;
	}
	return bin->fd;
}
