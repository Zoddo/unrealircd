/*
 *   Unreal Internet Relay Chat Daemon, src/s_conf2.c
 *   (C) 1998-2000 Chris Behrens & Fred Jacobs
 *   Modified by the UnrealIRCd team
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "channel.h"
#include <fcntl.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/wait.h>
#else
#include <io.h>
#endif
#include <sys/stat.h>
#ifdef __hpux
#include "inet.h"
#endif
#if defined(PCS) || defined(AIX) || defined(SVR3)
#include <time.h>
#endif
#include <string.h>

#include "h.h"

typedef struct _confcommand ConfigCommand;
struct	_confcommand
{
	char	*name;
	int	(*func)(ConfigFile *conf, ConfigEntry *ce);
};

int	_conf_admin(ConfigFile *conf, ConfigEntry *ce);
int	_conf_me(ConfigFile *conf, ConfigEntry *ce);
int	_conf_oper(ConfigFile *conf, ConfigEntry *ce);

static ConfigCommand _ConfigCommands[] = {
	{ "admin", _conf_admin },
	{ "me", _conf_me },
	{ "oper", _conf_oper },
	{ NULL, NULL  }
};

void config_free(ConfigFile *cfptr);
ConfigFile *config_load(char *filename);
ConfigEntry *config_find(ConfigEntry *ceptr, char *name);
static void config_error(char *format, ...);
static ConfigFile *config_parse(char *filename, char *confdata);
static void config_entry_free(ConfigEntry *ceptr);
int	ConfigParse(ConfigFile *cfptr);

static void config_error(char *format, ...)
{
	va_list		ap;
	char		buffer[1024];
	char		*ptr;

	va_start(ap, format);
	vsprintf(buffer, format, ap);
	va_end(ap);
	if ((ptr = strchr(buffer, '\n')) != NULL)
		*ptr = '\0';
	fprintf(stderr, "%s\n", buffer);
}

static void config_status(char *format, ...)
{
	va_list		ap;
	char		buffer[1024];
	char		*ptr;

	va_start(ap, format);
	vsprintf(buffer, format, ap);
	va_end(ap);
	if ((ptr = strchr(buffer, '\n')) != NULL)
		*ptr = '\0';
	fprintf(stderr, "%s\n", buffer);
}

static ConfigFile *config_parse(char *filename, char *confdata)
{
	char		*ptr;
	char		*start;
	int			linenumber = 1;
	ConfigEntry	*curce;
	ConfigEntry	**lastce;
	ConfigEntry	*cursection;

	ConfigFile	*curcf;
	ConfigFile	*lastcf;

	lastcf = curcf = (ConfigFile *)malloc(sizeof(ConfigFile));
	memset(curcf, 0, sizeof(ConfigFile));
	curcf->cf_filename = strdup(filename);
	lastce = &(curcf->cf_entries);
	curce = NULL;
	cursection = NULL;
	for(ptr=confdata;*ptr;ptr++)
	{
		switch(*ptr)
		{
			case ';':
				if (!curce)
				{
					config_error("%s:%i Ignoring extra semicolon\n",
						filename, linenumber);
					break;
				}
				if (!strcmp(curce->ce_varname, "include"))
				{
					ConfigFile	*cfptr;

					if (!curce->ce_vardata)
					{
						config_error("%s:%i Ignoring \"include\": No filename given\n",
							filename, linenumber);
						config_entry_free(curce);
						curce = NULL;
						continue;
					}
					if (strlen(curce->ce_vardata) > 255)
						curce->ce_vardata[255] = '\0';
					cfptr = config_load(curce->ce_vardata);
					if (cfptr)
					{
						lastcf->cf_next = cfptr;
						lastcf = cfptr;
					}
					config_entry_free(curce);
					curce = NULL;
					continue;
				}
				*lastce = curce;
				lastce = &(curce->ce_next);
				curce->ce_fileposend = (ptr - confdata);
				curce = NULL;
				break;
			case '{':
				if (!curce)
				{
					config_error("%s:%i: No name for section start\n",
							filename, linenumber);
					continue;
				}
				else if (curce->ce_entries)
				{
					config_error("%s:%i: Ignoring extra section start\n",
							filename, linenumber);
					continue;
				}
				curce->ce_sectlinenum = linenumber;
				lastce = &(curce->ce_entries);
				cursection = curce;
				curce = NULL;
				break;
			case '}':
				if (curce)
				{
					config_error("%s:%i: Missing semicolon before close brace\n",
						filename, linenumber);
					config_entry_free(curce);
					config_free(curcf);
					return NULL;
				}
				else if (!cursection)
				{
					config_error("%s:%i: Ignoring extra close brace\n",
						filename, linenumber);
					continue;
				}
				curce = cursection;
				cursection->ce_fileposend = (ptr - confdata);
				cursection = cursection->ce_prevlevel;
				if (!cursection)
					lastce = &(curcf->cf_entries);
				else
					lastce = &(cursection->ce_entries);
				for(;*lastce;lastce = &((*lastce)->ce_next))
					continue;
				break;
			case '/':
				if (*(ptr+1) == '/')
				{
					ptr += 2;
					while(*ptr && (*ptr != '\n'))
						ptr++;
					if (!*ptr)
						break;
					ptr--; /* grab the \n on next loop thru */
					continue;
				}
				else if (*(ptr+1) == '*')
				{
					int commentstart = linenumber;

					for(ptr+=2;*ptr;ptr++)
					{
						if ((*ptr == '*') && (*(ptr+1) == '/'))
						{
							ptr++;
							break;
						}
						else if (*ptr == '\n')
							linenumber++;
					}
					if (!*ptr)
					{
						config_error("%s:%i Comment on this line does not end\n",
							filename, commentstart);
						config_entry_free(curce);
						config_free(curcf);
						return NULL;
					}
				}
				break;
			case '\"':
				start = ++ptr;
				for(;*ptr;ptr++)
				{
					if ((*ptr == '\\') && (*(ptr+1) == '\"'))
					{
						char *tptr = ptr;
						while((*tptr = *(tptr+1)))
							tptr++;
					}
					else if ((*ptr == '\"') || (*ptr == '\n'))
						break;
				}
				if (!*ptr || (*ptr == '\n'))
				{
					config_error("%s:%i: Unterminated quote found\n",
							filename, linenumber);
					config_entry_free(curce);
					config_free(curcf);
					return NULL;
				}
				if (curce)
				{
					if (curce->ce_vardata)
					{
						config_error("%s:%i: Ignoring extra data\n",
							filename, linenumber);
					}
					else
					{
						curce->ce_vardata = (char *)malloc(ptr-start+1);
						strncpy(curce->ce_vardata, start, ptr-start);
						curce->ce_vardata[ptr-start] = '\0';
						curce->ce_vardatanum = atoi(curce->ce_vardata);
					}
				}
				else
				{
					curce = (ConfigEntry *)malloc(sizeof(ConfigEntry));
					memset(curce, 0, sizeof(ConfigEntry));
					curce->ce_varname = (char *)malloc(ptr-start+1);
					strncpy(curce->ce_varname, start, ptr-start);
					curce->ce_varname[ptr-start] = '\0';
					curce->ce_varlinenum = linenumber;
					curce->ce_fileptr = curcf;
					curce->ce_prevlevel = cursection;
					curce->ce_fileposstart = (start - confdata);
				}
				break;
			case '\n':
				linenumber++;
				if (*(ptr+1) == '#')
				{
					ptr += 2;
					while(*ptr && (*ptr != '\n'))
						ptr++;
					if (*ptr == '\n')
					{
						ptr--;
						continue;
					}
				}
				/* fall through */
			case '\t':
			case ' ':
			case '\r':
				break;
			default:
				if ((*ptr == '*') && (*(ptr+1) == '/'))
				{
					config_error("%s:%i Ignoring extra end comment\n",
						filename, linenumber);
					ptr++;
					break;
				}
				start = ptr;
				for(;*ptr;ptr++)
				{
					if ((*ptr == ' ') || (*ptr == '\t') || (*ptr == '\n') || (*ptr == ';'))
						break;
				}
				if (!*ptr)
				{
					if (curce)
						config_error("%s: Unexpected EOF for variable starting at %i\n",
							filename, curce->ce_varlinenum);
					else if (cursection)
						config_error("%s: Unexpected EOF for section starting at %i\n",
							filename, curce->ce_sectlinenum);
					else
						config_error("%s: Unexpected EOF.\n", filename);
					config_entry_free(curce);
					config_free(curcf);
					return NULL;
				}
				if (curce)
				{
					if (curce->ce_vardata)
					{
						config_error("%s:%i: Ignoring extra data\n",
							filename, linenumber);
					}
					else
					{
						curce->ce_vardata = (char *)malloc(ptr-start+1);
						strncpy(curce->ce_vardata, start, ptr-start);
						curce->ce_vardata[ptr-start] = '\0';
						curce->ce_vardatanum = atoi(curce->ce_vardata);
					}
				}
				else
				{
					curce = (ConfigEntry *)malloc(sizeof(ConfigEntry));
					memset(curce, 0, sizeof(ConfigEntry));
					curce->ce_varname = (char *)malloc(ptr-start+1);
					strncpy(curce->ce_varname, start, ptr-start);
					curce->ce_varname[ptr-start] = '\0';
					curce->ce_varlinenum = linenumber;
					curce->ce_fileptr = curcf;
					curce->ce_prevlevel = cursection;
					curce->ce_fileposstart = (start - confdata);
				}
				if ((*ptr == ';') || (*ptr == '\n'))
					ptr--;
				break;
		} /* switch */
	} /* for */
	if (curce)
	{
		config_error("%s: Unexpected EOF for variable starting on line %i\n",
			filename, curce->ce_varlinenum);
		config_entry_free(curce);
		config_free(curcf);
		return NULL;
	}
	else if (cursection)
	{
		config_error("%s: Unexpected EOF for section starting on line %i\n",
				filename, cursection->ce_sectlinenum);
		config_free(curcf);
		return NULL;
	}
	return curcf;
}

static void config_entry_free(ConfigEntry *ceptr)
{
	ConfigEntry	*nptr;

	for(;ceptr;ceptr=nptr)
	{
		nptr = ceptr->ce_next;
		if (ceptr->ce_entries)
			config_entry_free(ceptr->ce_entries);
		if (ceptr->ce_varname)
			free(ceptr->ce_varname);
		if (ceptr->ce_vardata)
			free(ceptr->ce_vardata);
		free(ceptr);
	}
}

void config_free(ConfigFile *cfptr)
{
	ConfigFile	*nptr;

	for(;cfptr;cfptr=nptr)
	{
		nptr = cfptr->cf_next;
		if (cfptr->cf_entries)
			config_entry_free(cfptr->cf_entries);
		if (cfptr->cf_filename)
			free(cfptr->cf_filename);
		free(cfptr);
	}
}

ConfigFile *config_load(char *filename)
{
	struct stat sb;
	int			fd;
	int			ret;
	char		*buf = NULL;
	ConfigFile	*cfptr;

	fd = open(filename, O_RDONLY);
	if (fd == -1)
	{
		config_error("Couldn't open \"%s\": %i\n", filename, errno);
		return NULL;
	}
	if (fstat(fd, &sb) == -1)
	{
		config_error("Couldn't fstat \"%s\": %i\n", filename, errno);
		close(fd);
		return NULL;
	}
	if (!sb.st_size)
	{
		close(fd);
		return NULL;
	}
	buf = (char *)malloc(sb.st_size+1);
	if (buf == NULL)
	{
		config_error("Out of memory trying to load \"%s\"\n", filename);
		close(fd);
		return NULL;
	}
	ret = read(fd, buf, sb.st_size);
	if (ret != sb.st_size)
	{
		config_error("Error reading \"%s\": %i\n", filename,
			ret == -1 ? errno : EFAULT);
		free(buf);
		close(fd);
		return NULL;
	}
	buf[ret] = '\0';
	close(fd);
	cfptr = config_parse(filename, buf);
	free(buf);
	return cfptr;
}

ConfigEntry *config_find(ConfigEntry *ceptr, char *name)
{
	for(;ceptr;ceptr=ceptr->ce_next)
		if (!strcmp(ceptr->ce_varname, name))
			break;
	return ceptr;
}


int	init_conf2(char *filename)
{
	ConfigFile	*cfptr;
	int		i = 0;
	if (!filename)
	{
		config_error("Could not load config file %s", filename);
		return 0;
	}
	config_status("Opening config file %s .. ", filename);
	if (cfptr = config_load(filename))
	{
		config_status("Config file loaded without problems");
		i = ConfigParse(cfptr);
		config_free(cfptr);
		return i;
	}
	else
	{
		config_error("Could not load config file");
		return 0;
	}
}

int	ConfigCmd(ConfigFile *cf, ConfigEntry *ce, ConfigCommand *cc)
{
	ConfigEntry *cep;
	ConfigCommand *ccp;
	if (!ce)
	{
		config_error("ConfigCmd: Got !ce");
		return -1;
	}
	if (!cc)
	{
		config_error("ConfigCmd: Got !cc");
		return -1;
	}
	for (cep = ce; cep; cep = cep->ce_next)
	{
		if (!cep->ce_varname)
		{
			config_error("NULL cep->ce_varname");
			continue;	
		}
		config_status("Searching for command %s", cep->ce_varname);
		for (ccp = cc; ccp->name; ccp++)
		{
			if (!strcasecmp(ccp->name, cep->ce_varname))
			{
				config_status("Executing %s", cep->ce_varname);
				ccp->func(cf, cep);
			}
		}
	}
}

int	ConfigParse(ConfigFile *cfptr)
{
	ConfigEntry	*ce = NULL;
	
	ConfigCmd(cfptr, cfptr->cf_entries, _ConfigCommands);
}

/* Here is the command parsing instructions */

int	_conf_admin(ConfigFile *conf, ConfigEntry *ce)
{
	ConfigEntry *cep;
	
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!cep->ce_varname)
		{
			config_error("Blank admin line");
			continue;	
		}
		config_status("[admin] we got line: %s",
				cep->ce_varname);
	} 
}

int	_conf_me(ConfigFile *conf, ConfigEntry *ce)
{
	ConfigEntry *cep;
	
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!cep->ce_varname)
		{
			config_error("Blank me line");
			continue;	
		}
		config_status("[me] Set %s to %s",
				cep->ce_varname, cep->ce_vardata);
	} 
}

int	_conf_oper(ConfigFile *conf, ConfigEntry *ce)
{
	ConfigEntry *cep;
	ConfigEntry *cepp;
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!cep->ce_varname)
		{
			config_error("Blank oper line");
			continue;
		}
		if (!cep->ce_entries)
		{
			config_status("[oper] Set %s to %s",
				cep->ce_varname, cep->ce_vardata);
		}
		else
		{
			if (!strcmp(cep->ce_varname, "flags"))
			{
				for (cepp = cep->ce_entries; cepp; cepp = cepp->ce_next)
				{
					config_status("[oper] got flag %s", cepp->ce_varname);
				}	
				continue;
			}
		}

	} 
}

