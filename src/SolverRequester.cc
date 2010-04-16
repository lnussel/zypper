/*---------------------------------------------------------------------------*\
                          ____  _ _ __ _ __  ___ _ _
                         |_ / || | '_ \ '_ \/ -_) '_|
                         /__|\_, | .__/ .__/\___|_|
                             |__/|_|  |_|
\*---------------------------------------------------------------------------*/


/** \file SolverRequester.cc
 * 
 */

#include "zypp/ZYppFactory.h"
#include "zypp/base/LogTools.h"

#include "zypp/PoolQuery.h"
#include "zypp/PoolItemBest.h"

#include "zypp/Capability.h"
#include "zypp/Resolver.h"
#include "zypp/Patch.h"
#include "zypp/ui/Selectable.h"

#include "Zypper.h" // temporarily!!!
#include "misc.h"

#include "SolverRequester.h"

using namespace std;
using namespace zypp;
using namespace zypp::ui;


/////////////////////////////////////////////////////////////////////////
// SolverRequester::Options
/////////////////////////////////////////////////////////////////////////

void SolverRequester::Options::setForceByCap(bool value)
{
  if (value && force_by_name)
    DBG << "resetting previously set force_by_name" << endl;

  force_by_cap = value;
  force_by_name = !force_by_cap;
}

void SolverRequester::Options::setForceByName(bool value)
{
  if (value && force_by_cap)
    DBG << "resetting previously set force_by_cap" << endl;

  force_by_name = value;
  force_by_cap = !force_by_name;
}


/////////////////////////////////////////////////////////////////////////
// SolverRequester
/////////////////////////////////////////////////////////////////////////

void SolverRequester::install(const PackageArgs & args)
{
  _requested_inst = true;
  installRemove(args);
}

// ----------------------------------------------------------------------------

void SolverRequester::remove(const PackageArgs & args)
{
  if (args.options().do_by_default)
  {
    INT << "PackageArgs::Options::do_by_default == true."
        << " Set it to 'false' when doing 'remove'" << endl;
    return;
  }

  installRemove(args);
}

// ----------------------------------------------------------------------------

void SolverRequester::installRemove(const PackageArgs & args)
{
  if (args.empty())
    return;

  for_(it, args.doCaps().begin(), args.doCaps().end())
    install(it->first, it->second);

  // TODO solve before processing dontCaps? so that we could unset any
  // dontCaps that are already set for installation. This would allow
  //   $ zypper install pattern:lamp_sever -someunwantedpackage
  // and similar nice things.

  for_(it, args.dontCaps().begin(), args.dontCaps().end())
    remove(it->first);
}

// ----------------------------------------------------------------------------

/**
 * For given Capability & repo & Options:
 *
 * 1) if the --capability was not specified, try to install 'by name' first.
 *    I.e. via ui::Selectable and/or PoolItem.
 *    note: wildcards must be supported here, use PoolQuery with match_glob
 *          to find the selectables
 *
 * 2) if no package could be found by name and --name was not specified,
 *    or --capability was specified, install 'by capability',
 *    i.e. using ResPool::addRequires(cap)
 *
 * NOTES
 * - If the argument contains repository, issue request forcing the repo
 * - In both cases check for already installed packages, and if found, hand over
 *   to \ref updateTo(const Capability&, const string&, const PoolItem&) method.
 *
 * TODO
 * - maybe a check for glob wildcards in cap name would make sense before trying
 *   by-cap
 */
void SolverRequester::install(const Capability & cap, const string & repoalias)
{
  sat::Solvable::SplitIdent splid(cap.detail().name());
  ResKind capkind = splid.kind();
  string capname = splid.name().asString();

  // first try by name

  if (!_opts.force_by_cap)
  {
    PoolQuery q = pkg_spec_to_poolquery(cap, _opts.from_repos);
    if (!repoalias.empty())
      q.addRepo(repoalias);

    // get the best matching items and tag them for installation.
    // FIXME this ignores vendor lock - we need some way to do --from which
    // would respect vendor lock: e.g. a new Selectable::updateCandidateObj(Options&)
    PoolItemBest bestMatches(q.begin(), q.end());
    if (!bestMatches.empty())
    {
      for_(sit, bestMatches.begin(), bestMatches.end())
      {
        Selectable::Ptr s(asSelectable()(*sit));
        PoolItem instobj(s->installedObj());
        if (instobj)
        {
          // whether user requested specific repo/version/arch
          bool userconstraints =
              cap.detail().isVersioned() || cap.detail().hasArch()
              || !_opts.from_repos.empty() || !repoalias.empty();
          // check vendor (since PoolItemBest does not do it)
          bool changes_vendor = instobj->vendor() != (*sit)->vendor();

          PoolItem best;
          if (userconstraints)
            updateTo(cap, repoalias, *sit);
          else if ((best = s->updateCandidateObj()))
            updateTo(cap, repoalias, best);
          else if (changes_vendor)
            updateTo(cap, repoalias, instobj);
          else
            updateTo(cap, repoalias, *sit);
        }
        else if (_requested_inst)
        {
          setToInstall(*sit);
          MIL << "installing " << *sit << endl;
        }
        else
          addFeedback(Feedback::NOT_INSTALLED, cap, repoalias);
        // TODO handle patches (isBroken), patterns (isSatisfied instead of hasInstalledObj)
      }
      return;
    }
    else if (_opts.force_by_name)
    {
      addFeedback(Feedback::NOT_FOUND_NAME, cap, repoalias);
      WAR << "'" << cap << "' not found" << endl;
      return;
    }

    addFeedback(Feedback::NOT_FOUND_NAME_TRYING_CAPS, cap, repoalias);
  }

  // try by capability

  // is there a provider for the requested capability?
  sat::WhatProvides q(cap);
  if (q.empty())
  {
    addFeedback(Feedback::NOT_FOUND_CAP, cap, repoalias);
    WAR << str::form("'%s' not found", cap.asString().c_str()) << endl;
    return;
  }

  // is the provider already installed?
  set<PoolItem> providers = get_installed_providers(cap);
  // already installed, try to update()
  for_(it, providers.begin(), providers.end())
  {
    if (_requested_inst)
      addFeedback(Feedback::ALREADY_INSTALLED, cap, repoalias, *it, *it);
    MIL << "provider '" << *it << "' of '" << cap << "' installed" << endl;
  }

  if (providers.empty())
  {
    DBG << "adding requirement " << cap << endl;
    addRequirement(cap);
  }
}

// ----------------------------------------------------------------------------

/**
 * Remove packages based on given Capability & Options from the system.
 */
void SolverRequester::remove(const Capability & cap)
{
  sat::Solvable::SplitIdent splid(cap.detail().name());
  ResKind capkind = splid.kind();
  string capname = splid.name().asString();

  // first try by name

  if (!_opts.force_by_cap)
  {
    PoolQuery q = pkg_spec_to_poolquery(cap, "");

    if (!q.empty())
    {
      bool got_installed = false;
      for_(it, q.poolItemBegin(), q.poolItemEnd())
      {
        if (it->status().isInstalled())
        {
          DBG << "Marking for deletion: " << *it << endl;
          setToRemove(*it);
          got_installed = true;
        }
      }
      if (got_installed)
        return;
      else
      {
        addFeedback(Feedback::NOT_INSTALLED, cap);
        MIL << "'" << cap << "' is not installed" << endl;
        if (_opts.force_by_name)
          return;
      }
      // TODO handle patches (cannot uninstall!), patterns (remove content(?))
    }
    else if (_opts.force_by_name)
    {
      addFeedback(Feedback::NOT_FOUND_NAME, cap);
      WAR << "'" << cap << "' not found" << endl;
      return;
    }
  }

  // try by capability

  addFeedback(Feedback::NOT_FOUND_NAME_TRYING_CAPS, cap);

  // is there a provider for the requested capability?
  sat::WhatProvides q(cap);
  if (q.empty())
  {
    addFeedback(Feedback::NOT_FOUND_CAP, cap);
    WAR << str::form("'%s' not found", cap.asString().c_str()) << endl;
    return;
  }

  // is the provider already installed?
  set<PoolItem> providers = get_installed_providers(cap);

  // not installed, nothing to do
  if (providers.empty())
  {
    addFeedback(Feedback::NO_INSTALLED_PROVIDER, cap);
    MIL << "no provider of " << cap << "is installed" << endl;
  }
  else
  {
    MIL << "adding conflict " << cap << endl;
    addConflict(cap);
  }
}

// ----------------------------------------------------------------------------

void SolverRequester::update(const PackageArgs & args)
{
  if (args.empty())
    return;

  _requested_inst = false;

  for_(it, args.doCaps().begin(), args.doCaps().end())
    update(it->first, it->second);

  /* TODO Solve and unmark dont which are setToBeInstalled in the pool?
  for_(it, args.dontCaps().begin(), args.dontCaps().end())
    remove(it->first);
  */
}

// ----------------------------------------------------------------------------

/*
 * If at least one provider of given \a cap is already installed,
 * this method checks for available update candidates and tries to select
 * the best for installation (thus update). Reports any problems or interesting
 * info back to user.
 *
 * If no provider is installed, it does no action other than report the fact.
 *
 * NOTES
 * - If the argument contains repository, issue request forcing the repo and
 *   - if --force was specified, insist on installing from that repo (even
 *     downgrade or change vendor or low priority) TODO
 *   - if --force was NOT used, only upgrade without vendor change or priority
 *     violation. If downgrade or vendor change is needed to get the highest
 *     version, report back to user and advice to use --force,
 *     if that's what is really wanted. TODO
 */
void SolverRequester::update(const Capability & cap, const string & repoalias)
{
  // FIXME this sucks
  bool reqwasinst = _requested_inst;
  _requested_inst = false;
  install(cap, repoalias);
  _requested_inst = reqwasinst;
}

// ----------------------------------------------------------------------------

void SolverRequester::updatePatterns()
{

}

// ----------------------------------------------------------------------------

void SolverRequester::updatePatches()
{
  DBG << "going to mark needed patches for installation" << endl;

  // search twice: if there are none with restartSuggested(), retry on all
  // (in the first run, ignore_pkgmgmt == 0, in the second it is 1)
  bool any_marked = false;
  for (unsigned ignore_pkgmgmt = 0;
       !any_marked && ignore_pkgmgmt < 2; ++ignore_pkgmgmt)
  {
    for_(it, zypp::getZYpp()->pool().proxy().byKindBegin(ResKind::patch),
             zypp::getZYpp()->pool().proxy().byKindEnd  (ResKind::patch))
    {
      if (installPatch((*it)->name(), ignore_pkgmgmt))
        any_marked = true;
    }

    if (any_marked && !ignore_pkgmgmt)
      MIL << "got some pkgmgmt patches, will install these first" << endl;
  }
}

// ----------------------------------------------------------------------------

bool SolverRequester::installPatch(const string & name, bool ignore_pkgmgmt)
{
#warning get rid of zypper here
  Zypper & zypper = *Zypper::instance();
  Selectable::Ptr s = Selectable::get(ResKind::patch, name);

  Patch::constPtr patch = asKind<Patch>(s->candidateObj());
  if (s->isBroken()) // bnc #506860
  {
    DBG << "broken candidate patch " << patch
        << " affects_pkgmgmt: " << patch->restartSuggested()
        << (ignore_pkgmgmt ? " (ignored)" : "") << endl;

    if (ignore_pkgmgmt || patch->restartSuggested())
    {
      // bnc #221476
      if (_opts.skip_interactive
          && (patch->interactive() || !patch->licenseToConfirm().empty()))
      {
        // Skipping a patch because it is marked as interactive or has
        // license to confirm and --skip-interactive is requested
        // (i.e. also --non-interactive, since it implies --skip-interactive)
        zypper.out().warning(str::form(
          // translators: %s is the name of a patch
          _("'%s' is interactive, skipping."),
          string(patch->name() + string("-") + patch->edition().asString()).c_str()));
        return false;
      }
      else
      {
        // TODO use _opts.force
        bool result = s->setToInstall();
        if (!result)
          ERR << "Marking " << *s << " for installation failed" << endl;
        return result;
      }
    }
  }
  else
    XXX << "candidate patch " << patch << " is satisfied or irrelevant" << endl;

  return false;
}

// ----------------------------------------------------------------------------

void SolverRequester::updateTo(
      const Capability & cap, const string & repoalias, const PoolItem & selected)
{
  if (!selected)
  {
    INT << "Candidate is empty, returning! Pass PoolItem you want to update to."
        << endl;
    return;
  }

#warning get rid of zypper here
  Zypper & zypper = *Zypper::instance();
  Selectable::Ptr s = asSelectable()(selected);

  // the best object without repository, arch, or version restriction
  PoolItem theone = s->updateCandidateObj();
  // the best installed object
  PoolItem installed = s->installedObj();
  // highest available version
  PoolItem highest = s->highestAvailableVersionObj();

  if (!installed)
  {
    INT << "no installed object, nothing to update, returning" << endl;
    return;
#warning TODO handle pseudoinstalled objects
  }

  DBG << "selected:  " << selected << endl;
  DBG << "best:      " << theone    << endl;
  DBG << "highest:   " << highest   << endl;
  DBG << "installed: " << installed << endl;


  // ******* request ********

  if (!identical(installed, selected))
  {
    if (_opts.best_effort)
    {
      // require version greater than than the one installed
      Capability c(s->name(), Rel::GT, installed->edition(), s->kind());
      addRequirement(c);
      MIL << *s << " update: adding requirement " << c << endl;
    }
    else if (selected->edition() > installed->edition())
    {
      // set 'candidate' for installation
      setToInstall(selected);
      MIL << *s << " update: setting " << selected << " to install" << endl;
    }
    else if (_opts.force)
    {
      // set 'candidate' for installation
      setToInstall(selected);
      MIL << *s << " update: setting " << selected << " to install" << endl;
    }
  }


  // ******* report ********

  // the candidate is already installed
  if (identical(installed, selected))
  {
    // only say 'already installed' in case of install, if update was requested
    // only report if we fail to install the newest version (the code below)
    if (_requested_inst)
    {
      addFeedback(
          Feedback::ALREADY_INSTALLED, cap, repoalias, selected, installed);
      MIL << "'" << cap << "'";
      if (!repoalias.empty())
        MIL << " from '" << repoalias << "'";
      MIL << " already installed." << endl;
    }
    // TODO other kinds

    // no available object (bnc #591760)
    // !availableEmpty() <=> theone && highest
    if (s->availableEmpty())
    {
      addFeedback(Feedback::NO_UPD_CANDIDATE, cap, repoalias, PoolItem(), installed);
      DBG << "no available objects in repos, skipping update of " << s->name() << endl;
      return;
    }

    // the highest version is already there
    if (identical(installed, highest) || highest->edition() < installed->edition())
      addFeedback(Feedback::NO_UPD_CANDIDATE, cap, repoalias, selected, installed);
  }
  else if (installed->edition() > selected->edition())
  {
    zypper.out().info(
        str::form(_(
            "The selected package '%s-%s.%s' from repository '%s' has lower"
            " version than the installed one."),
            selected->name().c_str(),
            selected->edition().asString().c_str(),
            selected->arch().asString().c_str(),
            zypper.config().show_alias ?
                selected->repoInfo().alias().c_str() :
                selected->repoInfo().name().c_str()));
    zypper.out().info(str::form(
        _("Use '%s' to force installation of the package."), "--force"));
  }

  // there is higher version available than the selected candidate
  // this can happen because of repo priorities, locks, vendor lock, and
  // because of CLI restrictions: repos/arch/version (bnc #522223)
  if (!identical(selected, highest) && highest->edition() > installed->edition())
  {
    // whether user requested specific repo/version/arch
    bool userconstraints =
        cap.detail().isVersioned() || cap.detail().hasArch()
        || !_opts.from_repos.empty() || !repoalias.empty();
    if (userconstraints)
    {
      addFeedback(Feedback::UPD_CANDIDATE_USER_RESTRICTED, cap, repoalias, selected, installed);
      DBG << "Newer object exists, but has different repo/arch/version: " << highest << endl;
    }

    // update candidate locked
    if (s->status() == ui::S_Protected || highest.status().isLocked())
    {
      DBG << "Newer object exists, but is locked: " << highest << endl;

      ostringstream cmdhint;
      cmdhint << "zypper removelock " << highest->name();

      zypper.out().info(str::form(
        _("There is an update candidate for '%s', but it is locked."
          " Use '%s' to unlock it."),
        s->name().c_str(), cmdhint.str().c_str()));
    }

    // update candidate has different vendor
    if (highest->vendor() != installed->vendor())
    {
      addFeedback(Feedback::UPD_CANDIDATE_CHANGES_VENDOR, cap, repoalias, selected, installed);
      DBG << "Newer object with different vendor exists: " << highest
          << " (" << highest->vendor() << ")"
          << ". Installed vendor: " << installed->vendor() << endl;
    }

    // update candidate is from low-priority (higher priority number) repo
    if (highest->repoInfo().priority() > selected->repoInfo().priority())
    {
      addFeedback(Feedback::UPD_CANDIDATE_HAS_LOWER_PRIO, cap, repoalias, selected, installed);
      DBG << "Newer object exists in lower-priority repo: " << highest << endl;
    }
  } // !identical(selected, highest) && highest->edition() > installed->edition()
}

// ----------------------------------------------------------------------------

void SolverRequester::setToInstall(const PoolItem & pi)
{
  // pi.status().setToBeInstalled(ResStatus::USER); ?
  asSelectable()(pi)->setOnSystem(pi, ResStatus::USER);
  addFeedback(Feedback::SET_TO_INSTALL, Capability(), "", pi);
  _toinst.insert(pi);
}

// ----------------------------------------------------------------------------

void SolverRequester::setToRemove(const zypp::PoolItem & pi)
{
  pi.status().setToBeUninstalled(ResStatus::USER);
  addFeedback(Feedback::SET_TO_REMOVE, Capability(), "", pi);
  _toremove.insert(pi);
}

// ----------------------------------------------------------------------------

void SolverRequester::addRequirement(const zypp::Capability & cap)
{
  zypp::getZYpp()->resolver()->addRequire(cap);
  addFeedback(Feedback::ADDED_REQUIREMENT, cap);
  _requires.insert(cap);
}

// ----------------------------------------------------------------------------

void SolverRequester::addConflict(const zypp::Capability & cap)
{
  zypp::getZYpp()->resolver()->addConflict(cap);
  addFeedback(Feedback::ADDED_CONFLICT, cap);
  _conflicts.insert(cap);
}

// ----------------------------------------------------------------------------

bool SolverRequester::hasFeedback(const Feedback::Id id) const
{
  for_(fb, _feedback.begin(), _feedback.end())
    if (fb->id() == id)
      return true;
  return false;
}
