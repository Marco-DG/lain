import styles from "./page.module.css";
import Hero from "../components/Hero";
import TerminalWindow from "../components/TerminalWindow";
import FeatureGrid from "../components/FeatureGrid";

export default function Home() {
  return (
    <div className={styles.container}>
      <Hero />
      <TerminalWindow />
      <FeatureGrid />
    </div>
  );
}
